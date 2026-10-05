//! SanAnskateas: evens out San Andreas collision before Skate sees it.
//!
//! SA stores collision vertices in 1/128 m and builds streets, pavements and
//! handrails from separate pieces that rarely meet exactly. Skate's wheels
//! feel every millimetre step at speed, and its grinds follow every kink and
//! break in a rail. So, per world:
//! - Ground: vertices of different pieces closer than `WELD` are joined, and
//!   a vertex lying just off another walkable piece's edge is put on that
//!   edge, so seams have no step.
//! - Rails (found by `rails.rs`): pieces of one rail broken apart by a small
//!   gap are joined again, points that only wobble off a straight line are
//!   dropped, and real bends are rounded (more gently where the rail turns
//!   upward, the bend that tips a fast board's nose up).
//!
//! Nothing on the ground moves more than `WELD`, and no rail point more than
//! `RAIL_MAX_MOVE`: curbs, steps and ledges keep their shape.

use bevy::platform::collections::HashMap;

use bevy::math::{IVec3, Vec3};

/// Vertices of different pieces this close (metres) are one.
pub const WELD: f32 = 0.015;
/// Rail points within this of a straight line through their neighbours are on it.
pub const RAIL_WOBBLE: f32 = 0.025;
/// Rail ends this close and lined up within `RAIL_JOIN_COS` are one rail.
pub const RAIL_JOIN: f32 = 0.08;
pub const RAIL_JOIN_COS: f32 = 0.94; // about 20 degrees
/// Rounding of rail bends: the nominal radius where the rail turns upward
/// (a dip) and elsewhere, and how far a rounded bend may leave the corner.
pub const RAIL_RADIUS_DIP: f32 = 1.5;
pub const RAIL_RADIUS: f32 = 0.6;
pub const RAIL_MAX_MOVE: f32 = 0.04;
const RAIL_MAX_MOVE_CREST: f32 = 0.02;
/// Faces this upward are ground (as `rails.rs`).
const WALKABLE_Z: f32 = 0.65;

#[derive(Clone, Copy, Debug, Default)]
pub struct Report {
    pub welded: usize,
    pub snapped: usize,
    pub flushed: usize,
    pub max_move: f32,
    pub rails_joined: usize,
    pub rail_points_before: usize,
    pub rail_points_after: usize,
    pub rail_max_move: f32,
}

fn normal(t: &[Vec3; 3]) -> Vec3 {
    (t[1] - t[0]).cross(t[2] - t[0]).normalize_or_zero()
}

/// Drops triangles that repeat another one (same corners to the millimetre,
/// facing the same way): GTA's map often has the same piece more than once
/// in one spot (the Lombard test ledge four times over), and every copy
/// added its own contact against the board. Returns how many went.
pub fn drop_copies(tris: &mut Vec<[Vec3; 3]>, scale: f32) -> usize {
    let key = |t: &[Vec3; 3]| {
        let c = t.map(|v| (v / scale * 1000.).round().as_ivec3().to_array());
        // Started at the smallest corner, so the same face matches whichever
        // corner it was listed from; the order (the way it faces) is kept.
        let first = (0..3).min_by_key(|&i| c[i]).unwrap();
        [c[first], c[(first + 1) % 3], c[(first + 2) % 3]]
    };
    let before = tris.len();
    let mut seen = bevy::platform::collections::HashSet::with_capacity(before);
    tris.retain(|t| seen.insert(key(t)));
    before - tris.len()
}

/// The triangle's height straight over or under `q`, if `q` is within it seen from above.
fn height_at(t: &[Vec3; 3], q: Vec3) -> Option<f32> {
    let (a, b, c) = (t[0], t[1], t[2]);
    let d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if d.abs() < 1e-9 {
        return None;
    }
    let w0 = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / d;
    let w1 = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / d;
    let w2 = 1. - w0 - w1;
    (w0 >= 0. && w1 >= 0. && w2 >= 0.).then(|| w0 * a.z + w1 * b.z + w2 * c.z)
}

/// Joins near vertices and puts vertices lying just off a walkable edge onto
/// it. `scale` turns metres into the triangles' units.
pub fn weld(tris: &mut [[Vec3; 3]], scale: f32, report: &mut Report) {
    let tol = WELD * scale;
    let mut index: HashMap<[u32; 3], usize> = HashMap::default();
    let mut verts: Vec<Vec3> = Vec::new();
    let mut corners: Vec<usize> = Vec::with_capacity(tris.len() * 3);
    let mut walkable_vertex = Vec::new();
    for t in tris.iter() {
        let up = normal(t).z > WALKABLE_Z;
        for v in t {
            let i = *index.entry(v.to_array().map(f32::to_bits)).or_insert_with(|| {
                verts.push(*v);
                walkable_vertex.push(false);
                verts.len() - 1
            });
            walkable_vertex[i] |= up;
            corners.push(i);
        }
    }
    // Clusters: the first vertex of each stays, later ones within `tol` join it.
    let cell = |v: Vec3| (v / tol).floor().as_ivec3();
    let mut reps: HashMap<IVec3, Vec<usize>> = HashMap::default();
    let mut moved = verts.clone();
    let mut is_rep = vec![false; verts.len()];
    for i in 0..verts.len() {
        let c = cell(verts[i]);
        let mut best: Option<(usize, f32)> = None;
        for dx in -1..=1 {
            for dy in -1..=1 {
                for dz in -1..=1 {
                    for &r in reps.get(&(c + IVec3::new(dx, dy, dz))).into_iter().flatten() {
                        let d = verts[r].distance(verts[i]);
                        if d < tol && best.is_none_or(|(_, bd)| d < bd) {
                            best = Some((r, d));
                        }
                    }
                }
            }
        }
        match best {
            Some((r, d)) => {
                moved[i] = verts[r];
                report.welded += 1;
                report.max_move = report.max_move.max(d / scale);
            }
            None => {
                reps.entry(c).or_default().push(i);
                is_rep[i] = true;
            }
        }
    }
    // T-junctions: a walkable vertex just off another walkable triangle's
    // edge (not one of its own) goes onto the edge.
    let mut edges: Vec<(usize, usize)> = Vec::new();
    let mut seen: HashMap<(usize, usize), ()> = HashMap::default();
    for (k, t) in tris.iter().enumerate() {
        if normal(t).z <= WALKABLE_Z {
            continue;
        }
        for e in 0..3 {
            let (a, b) = (corners[k * 3 + e], corners[k * 3 + (e + 1) % 3]);
            let (a, b) = (a.min(b), a.max(b));
            if seen.insert((a, b), ()).is_none() {
                edges.push((a, b));
            }
        }
    }
    let edge_cell = 4.0 * scale;
    let ecell = |v: Vec3| ((v.x / edge_cell).floor() as i32, (v.y / edge_cell).floor() as i32);
    let mut edge_grid: HashMap<(i32, i32), Vec<usize>> = HashMap::default();
    for (n, &(a, b)) in edges.iter().enumerate() {
        let (lo, hi) = (moved[a].min(moved[b]) - Vec3::splat(tol), moved[a].max(moved[b]) + Vec3::splat(tol));
        let (x0, y0) = ecell(lo);
        let (x1, y1) = ecell(hi);
        for x in x0..=x1 {
            for y in y0..=y1 {
                edge_grid.entry((x, y)).or_default().push(n);
            }
        }
    }
    let mut snapped = moved.clone();
    for i in 0..verts.len() {
        if !is_rep[i] || !walkable_vertex[i] {
            continue;
        }
        let v = moved[i];
        let mut best: Option<(Vec3, f32)> = None;
        for &n in edge_grid.get(&ecell(v)).into_iter().flatten() {
            let (a, b) = edges[n];
            let (pa, pb) = (moved[a], moved[b]);
            if pa == v || pb == v {
                continue;
            }
            let ab = pb - pa;
            let len2 = ab.length_squared();
            if len2 < 1e-12 {
                continue;
            }
            let t = (v - pa).dot(ab) / len2;
            if !(0.02..=0.98).contains(&t) {
                continue;
            }
            let p = pa + ab * t;
            let d = p.distance(v);
            if d > 1e-5 * scale && d < tol && best.is_none_or(|(_, bd)| d < bd) {
                best = Some((p, d));
            }
        }
        if let Some((p, d)) = best {
            snapped[i] = p;
            report.snapped += 1;
            report.max_move = report.max_move.max(d / scale);
        }
    }
    // Overlaps: the open edge of a walkable piece lying just above or below
    // another walkable surface (a slab laid on the road) is put flush with it.
    let rep_of: Vec<usize> =
        (0..verts.len()).map(|i| if is_rep[i] { i } else { index[&moved[i].to_array().map(f32::to_bits)] }).collect();
    let walkable_tris: Vec<[usize; 3]> = (0..tris.len())
        .filter(|&k| normal(&tris[k]).z > WALKABLE_Z)
        .map(|k| [0, 1, 2].map(|e| rep_of[corners[k * 3 + e]]))
        .collect();
    let mut uses: HashMap<(usize, usize), u32> = HashMap::default();
    for t in &walkable_tris {
        for e in 0..3 {
            let (a, b) = (t[e].min(t[(e + 1) % 3]), t[e].max(t[(e + 1) % 3]));
            *uses.entry((a, b)).or_default() += 1;
        }
    }
    let mut open = vec![false; verts.len()];
    for (&(a, b), &n) in &uses {
        if n == 1 {
            open[a] = true;
            open[b] = true;
        }
    }
    let tcell = |v: Vec3| ((v.x / (4. * scale)).floor() as i32, (v.y / (4. * scale)).floor() as i32);
    let mut tri_grid: HashMap<(i32, i32), Vec<usize>> = HashMap::default();
    for (n, t) in walkable_tris.iter().enumerate() {
        let p = t.map(|i| snapped[i]);
        let (lo, hi) = (p[0].min(p[1]).min(p[2]), p[0].max(p[1]).max(p[2]));
        let ((x0, y0), (x1, y1)) = (tcell(lo), tcell(hi));
        for x in x0..=x1 {
            for y in y0..=y1 {
                tri_grid.entry((x, y)).or_default().push(n);
            }
        }
    }
    let mut flush = snapped.clone();
    for i in 0..verts.len() {
        if !is_rep[i] || !open[i] {
            continue;
        }
        let p = snapped[i];
        let mut best: Option<f32> = None;
        for &n in tri_grid.get(&tcell(p)).into_iter().flatten() {
            let t = walkable_tris[n];
            if t.contains(&i) {
                continue;
            }
            if let Some(z) = height_at(&t.map(|k| snapped[k]), p) {
                let d = z - p.z;
                if d.abs() > 1e-4 * scale && d.abs() < tol && best.is_none_or(|b| d.abs() < b.abs()) {
                    best = Some(d);
                }
            }
        }
        if let Some(d) = best {
            flush[i].z += d;
            report.flushed += 1;
        }
    }
    // Followers move with their cluster's representative.
    let mut snapped = flush;
    for i in 0..verts.len() {
        if !is_rep[i] {
            snapped[i] = snapped[rep_of[i]];
        }
        report.max_move = report.max_move.max(snapped[i].distance(verts[i]) / scale);
    }
    for (k, t) in tris.iter_mut().enumerate() {
        for e in 0..3 {
            t[e] = snapped[corners[k * 3 + e]];
        }
    }
}

/// Smooths rails (polylines in the triangles' units; `scale` turns metres
/// into them): rejoins small breaks, drops wobble, rounds bends.
pub fn smooth_rails(rails: Vec<Vec<Vec3>>, scale: f32, report: &mut Report) -> Vec<Vec<Vec3>> {
    report.rail_points_before += rails.iter().map(Vec::len).sum::<usize>();
    let joined = join_rails(rails, scale, report);
    let out: Vec<Vec<Vec3>> = joined
        .into_iter()
        .map(|r| {
            let simple = simplify(&r, RAIL_WOBBLE * scale);
            for p in &r { // how far the wobble went
                report.rail_max_move = report.rail_max_move.max(distance_to_polyline(*p, &simple) / scale);
            }
            round_bends(&simple, scale, report)
        })
        .collect();
    report.rail_points_after += out.iter().map(Vec::len).sum::<usize>();
    out
}

fn join_rails(mut rails: Vec<Vec<Vec3>>, scale: f32, report: &mut Report) -> Vec<Vec<Vec3>> {
    let reach = RAIL_JOIN * scale;
    rails.retain(|r| r.len() >= 2);
    let cell = |p: Vec3| (p / reach).floor().as_ivec3();
    // An end: its point and the way the rail leaves through it.
    let end = |r: &[Vec3], last: bool| {
        let n = r.len();
        if last { (r[n - 1], (r[n - 1] - r[n - 2]).normalize_or_zero()) } else { (r[0], (r[0] - r[1]).normalize_or_zero()) }
    };
    loop {
        let mut ends: HashMap<IVec3, Vec<(usize, bool)>> = HashMap::default();
        for (i, r) in rails.iter().enumerate() {
            for last in [false, true] {
                ends.entry(cell(end(r, last).0)).or_default().push((i, last));
            }
        }
        // Two ends close together, the rails leaving them in opposite ways (lined up).
        let mut pair = None;
        'search: for (i, r) in rails.iter().enumerate() {
            for last in [false, true] {
                let (p, out) = end(r, last);
                let c = cell(p);
                for dx in -1..=1 {
                    for dy in -1..=1 {
                        for dz in -1..=1 {
                            for &(j, jlast) in ends.get(&(c + IVec3::new(dx, dy, dz))).into_iter().flatten() {
                                let (q, qout) = end(&rails[j], jlast);
                                if j != i && p.distance(q) < reach && out.dot(-qout) >= RAIL_JOIN_COS {
                                    pair = Some(((i, last), (j, jlast)));
                                    break 'search;
                                }
                            }
                        }
                    }
                }
            }
        }
        let Some(((i, ilast), (j, jlast))) = pair else { return rails };
        // a runs into the joint, b runs on from it.
        let mut a = rails[i].clone();
        if !ilast {
            a.reverse();
        }
        let mut b = rails[j].clone();
        if jlast {
            b.reverse();
        }
        let (p, q) = (a[a.len() - 1], b[0]);
        let last = a.len() - 1;
        a[last] = (p + q) * 0.5;
        a.extend_from_slice(&b[1..]);
        report.rails_joined += 1;
        report.rail_max_move = report.rail_max_move.max(p.distance(q) * 0.5 / scale);
        let (lo, hi) = (i.min(j), i.max(j));
        rails.swap_remove(hi);
        rails.swap_remove(lo);
        rails.push(a);
    }
}

/// Douglas-Peucker: the fewest points within `tol` of the line.
fn simplify(points: &[Vec3], tol: f32) -> Vec<Vec3> {
    if points.len() < 3 {
        return points.to_vec();
    }
    let mut keep = vec![false; points.len()];
    keep[0] = true;
    keep[points.len() - 1] = true;
    let mut stack = vec![(0, points.len() - 1)];
    while let Some((a, b)) = stack.pop() {
        let mut far = (0, 0.0f32);
        for k in a + 1..b {
            let d = distance_to_segment(points[k], points[a], points[b]);
            if d > far.1 {
                far = (k, d);
            }
        }
        if far.1 > tol {
            keep[far.0] = true;
            stack.push((a, far.0));
            stack.push((far.0, b));
        }
    }
    points.iter().zip(keep).filter(|(_, k)| *k).map(|(p, _)| *p).collect()
}

fn distance_to_segment(p: Vec3, a: Vec3, b: Vec3) -> f32 {
    let ab = b - a;
    let t = if ab.length_squared() > 0. { ((p - a).dot(ab) / ab.length_squared()).clamp(0., 1.) } else { 0. };
    p.distance(a + ab * t)
}

fn distance_to_polyline(p: Vec3, line: &[Vec3]) -> f32 {
    line.windows(2).map(|w| distance_to_segment(p, w[0], w[1])).fold(f32::MAX, f32::min)
}

/// Each corner becomes a curve (a quadratic through the corner's tangent
/// points) sized by `RAIL_RADIUS*` and limited so the curve stays within
/// `RAIL_MAX_MOVE` of the corner and doesn't reach past half a segment.
fn round_bends(points: &[Vec3], scale: f32, report: &mut Report) -> Vec<Vec3> {
    if points.len() < 3 {
        return points.to_vec();
    }
    let mut out = vec![points[0]];
    for k in 1..points.len() - 1 {
        let (p0, p, p1) = (points[k - 1], points[k], points[k + 1]);
        let (d0, d1) = ((p - p0).normalize_or_zero(), (p1 - p).normalize_or_zero());
        let turn = d0.dot(d1).clamp(-1., 1.).acos();
        if turn < 0.5f32.to_radians() {
            out.push(p);
            continue;
        }
        let dip = d1.z > d0.z; // turning upward: the bend a fast board's nose hits
        let (radius, max_move) = if dip { (RAIL_RADIUS_DIP, RAIL_MAX_MOVE) } else { (RAIL_RADIUS, RAIL_MAX_MOVE_CREST) };
        let half = (turn * 0.5).sin().max(1e-4);
        let reach = (radius * scale * (turn * 0.5).tan())
            .min(2. * max_move * scale / half)
            .min(0.5 * p.distance(p0).min(p.distance(p1)));
        let (t0, t1) = (p - d0 * reach, p + d1 * reach);
        let steps = ((turn.to_degrees() / 5.).ceil() as usize).max(2);
        for s in 0..=steps {
            let u = s as f32 / steps as f32;
            out.push(t0 * (1. - u) * (1. - u) + p * 2. * u * (1. - u) + t1 * u * u);
        }
        report.rail_max_move = report.rail_max_move.max(0.5 * reach * half / scale);
    }
    out.push(points[points.len() - 1]);
    out.dedup_by(|a, b| a.distance(*b) < 1e-4 * scale);
    out
}

/// Measures of how rough a world is, for comparing before and after.
#[derive(Clone, Copy, Debug, Default)]
pub struct Roughness {
    /// Seams between walkable surfaces checked, and steps across them.
    pub seams: usize,
    pub steps_2mm: usize,
    pub steps_5mm: usize,
    pub steps_10mm: usize,
    pub biggest_step: f32,
    pub rails: usize,
    pub rail_length: f32,
    /// Rail joints turning more than 2 degrees, and how many turn upward.
    pub bends: usize,
    pub dips: usize,
    pub sharpest: f32,
    /// Rail ends with another rail's end lined up within `RAIL_JOIN`.
    pub breaks: usize,
}

/// Steps across walkable seams (in metres, triangles in metres) and rail shape.
pub fn roughness(tris: &[[Vec3; 3]], rails: &[Vec<Vec3>]) -> Roughness {
    let mut r = Roughness::default();
    let walkable: Vec<usize> = (0..tris.len()).filter(|&i| normal(&tris[i]).z > WALKABLE_Z).collect();
    let cell = |x: f32| (x / 2.).floor() as i32;
    let mut grid: HashMap<(i32, i32), Vec<usize>> = HashMap::default();
    for &i in &walkable {
        let t = &tris[i];
        let (lo, hi) = (t[0].min(t[1]).min(t[2]), t[0].max(t[1]).max(t[2]));
        for x in cell(lo.x)..=cell(hi.x) {
            for y in cell(lo.y)..=cell(hi.y) {
                grid.entry((x, y)).or_default().push(i);
            }
        }
    }
    for &i in &walkable {
        let t = &tris[i];
        let n = normal(t);
        let centroid = (t[0] + t[1] + t[2]) / 3.;
        for e in 0..3 {
            let (a, b) = (t[e], t[(e + 1) % 3]);
            let m = (a + b) * 0.5;
            let mut out = (b - a).cross(n).normalize_or_zero();
            if out.dot(centroid - m) > 0. {
                out = -out;
            }
            let q = m + out * 0.002; // just across: a change of slope alone barely registers
            let own = t[0].z - (n.x * (q.x - t[0].x) + n.y * (q.y - t[0].y)) / n.z;
            let mut best: Option<f32> = None;
            for &j in grid.get(&(cell(q.x), cell(q.y))).into_iter().flatten() {
                if j == i {
                    continue;
                }
                if let Some(z) = height_at(&tris[j], q) {
                    let step = z - own;
                    if step.abs() < 0.05 && best.is_none_or(|s| step.abs() < s.abs()) {
                        best = Some(step);
                    }
                }
            }
            if let Some(step) = best {
                r.seams += 1;
                let s = step.abs();
                r.steps_2mm += usize::from(s > 0.002);
                r.steps_5mm += usize::from(s > 0.005);
                r.steps_10mm += usize::from(s > 0.010);
                r.biggest_step = r.biggest_step.max(s);
            }
        }
    }
    r.rails = rails.len();
    for rail in rails {
        r.rail_length += rail.windows(2).map(|w| w[0].distance(w[1])).sum::<f32>();
        for k in 1..rail.len().saturating_sub(1) {
            let (d0, d1) = ((rail[k] - rail[k - 1]).normalize_or_zero(), (rail[k + 1] - rail[k]).normalize_or_zero());
            let turn = d0.dot(d1).clamp(-1., 1.).acos().to_degrees();
            if turn > 2. {
                r.bends += 1;
                r.dips += usize::from(d1.z > d0.z + 0.01);
            }
            r.sharpest = r.sharpest.max(turn);
        }
    }
    let ends: Vec<(Vec3, Vec3)> = rails
        .iter()
        .filter(|rail| rail.len() >= 2)
        .flat_map(|rail| {
            let n = rail.len();
            [(rail[0], (rail[0] - rail[1]).normalize_or_zero()), (rail[n - 1], (rail[n - 1] - rail[n - 2]).normalize_or_zero())]
        })
        .collect();
    for (k, (p, d)) in ends.iter().enumerate() {
        r.breaks += usize::from(ends.iter().enumerate().any(|(m, (q, e))| {
            m / 2 != k / 2 && p.distance(*q) < RAIL_JOIN && d.dot(-*e) >= RAIL_JOIN_COS
        }));
    }
    r.breaks /= 2;
    r
}

#[cfg(test)]
mod tests {
    use super::*;

    const INCH: f32 = 1.0 / 0.0254;

    /// Worlds saved in game by the test command `dumpworld` (never published).
    fn load(name: &str) -> Option<Vec<[Vec3; 3]>> {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../test-worlds").join(format!("{name}.tris"));
        let d = std::fs::read(path).ok()?;
        if d.len() < 24 || &d[..8] != b"SKTRIS01" {
            return None;
        }
        let count = u32::from_le_bytes(d[20..24].try_into().ok()?) as usize;
        let f = |k: usize| f32::from_le_bytes(d[24 + k * 4..28 + k * 4].try_into().unwrap());
        (d.len() >= 24 + count * 36).then(|| {
            (0..count)
                .map(|t| [0, 1, 2].map(|v| Vec3::new(f(t * 9 + v * 3), f(t * 9 + v * 3 + 1), f(t * 9 + v * 3 + 2))))
                .collect()
        })
    }

    fn rails_of(tris: &[[Vec3; 3]]) -> Vec<Vec<Vec3>> {
        let inches: Vec<[Vec3; 3]> = tris.iter().map(|t| t.map(|v| v * INCH)).collect();
        crate::rails::find(&inches).0.into_iter().map(|r| r.into_iter().map(|p| p / INCH).collect()).collect()
    }

    #[test]
    fn worlds_before_and_after() {
        for name in ["lombard", "calton", "pershing", "runway"] {
            let Some(mut tris) = load(name) else {
                println!("{name}: no test world (dumpworld in game)");
                continue;
            };
            let original_rails = rails_of(&tris);
            let before = roughness(&tris, &original_rails);
            let mut report = Report::default();
            let t0 = std::time::Instant::now();
            weld(&mut tris, 1.0, &mut report);
            let t1 = std::time::Instant::now();
            let found = rails_of(&tris);
            let t2 = std::time::Instant::now();
            let rails = smooth_rails(found, 1.0, &mut report);
            let t3 = std::time::Instant::now();
            println!("  ms: weld {:.1}, find rails {:.1}, smooth rails {:.1}", (t1 - t0).as_secs_f32() * 1e3,
                (t2 - t1).as_secs_f32() * 1e3, (t3 - t2).as_secs_f32() * 1e3);
            let after = roughness(&tris, &rails);
            // Every stretch of the old rails must still have a rail under it.
            let (mut samples, mut kept, mut lost) = (0, 0, 0.0f32);
            for rail in &original_rails {
                for w in rail.windows(2) {
                    let n = (w[0].distance(w[1]) / 0.25).ceil().max(1.) as usize;
                    for s in 0..=n {
                        let p = w[0].lerp(w[1], s as f32 / n as f32);
                        samples += 1;
                        if rails.iter().any(|r| distance_to_polyline(p, r) < 0.06) {
                            kept += 1;
                        } else {
                            lost += 0.25;
                        }
                    }
                }
            }
            println!("== {name}: {} triangles", tris.len());
            println!("  before: {before:?}");
            println!("  after:  {after:?}");
            println!("  report: {report:?}");
            println!("  old rails still under a rail: {:.2}% ({lost:.1} m not)", 100. * kept as f32 / samples.max(1) as f32);
            // Which step loses them: the rail smoothing alone, on the unwelded world?
            let alone = smooth_rails(original_rails.clone(), 1.0, &mut Report::default());
            let welded_only = rails_of(&tris);
            let missing = |against: &[Vec<Vec3>]| -> (f32, Vec<(Vec3, f32)>) {
                let (mut lost, mut where_) = (0.0f32, Vec::new());
                for rail in &original_rails {
                    let mut gone = 0.0f32;
                    for w in rail.windows(2) {
                        let n = (w[0].distance(w[1]) / 0.25).ceil().max(1.) as usize;
                        for s in 0..=n {
                            if !against.iter().any(|r| distance_to_polyline(w[0].lerp(w[1], s as f32 / n as f32), r) < 0.06) {
                                gone += 0.25;
                            }
                        }
                    }
                    if gone > 0. {
                        lost += gone;
                        where_.push((rail[0], gone));
                    }
                }
                (lost, where_)
            };
            let (by_smoothing, _) = missing(&alone);
            let (by_weld, places) = missing(&welded_only);
            println!("  lost by rail smoothing alone: {by_smoothing:.1} m; by welding alone: {by_weld:.1} m");
            for (p, m) in places.iter().filter(|(_, m)| *m >= 2.).take(8) {
                println!("    {m:.1} m of rail near {:.1} {:.1} {:.1}", p.x, p.y, p.z);
            }
            assert!(report.max_move <= 2. * WELD + 1e-4, "{name}: ground moved {}", report.max_move); // joined, then put on an edge
            assert!(report.rail_max_move <= RAIL_MAX_MOVE.max(RAIL_JOIN * 0.5) + 1e-3, "{name}: rail moved {}", report.rail_max_move);
        }
    }

    /// Prints the geometry around a spot before and after welding
    /// (SK_PROBE="x y z name" in the environment).
    #[test]
    fn probe_a_spot() {
        let Ok(spec) = std::env::var("SK_PROBE") else { return };
        let w: Vec<&str> = spec.split_whitespace().collect();
        let at = Vec3::new(w[0].parse().unwrap(), w[1].parse().unwrap(), w[2].parse().unwrap());
        let Some(tris) = load(w[3]) else { return };
        let mut welded = tris.clone();
        weld(&mut welded, 1.0, &mut Report::default());
        for (k, (t, u)) in tris.iter().zip(&welded).enumerate() {
            if t.iter().any(|v| v.distance(at) < 0.6) {
                let n = normal(t);
                println!("tri {k} n=({:.2} {:.2} {:.2})", n.x, n.y, n.z);
                for e in 0..3 {
                    let d = u[e] - t[e];
                    println!("   {:.3} {:.3} {:.3}{}", t[e].x, t[e].y, t[e].z,
                        if d.length() > 1e-5 { format!("  -> moved {:.1} {:.1} {:.1} mm", d.x * 1000., d.y * 1000., d.z * 1000.) } else { String::new() });
                }
            }
        }
        for r in rails_of(&tris).iter().filter(|r| r.iter().any(|p| p.distance(at) < 1.)) {
            println!("rail before: {} points from {:?} to {:?}", r.len(), r[0], r[r.len() - 1]);
        }
        for r in rails_of(&welded).iter().filter(|r| r.iter().any(|p| p.distance(at) < 1.)) {
            println!("rail after: {} points from {:?} to {:?}", r.len(), r[0], r[r.len() - 1]);
        }
    }

    /// Long rails of a world as the game gets them (welded, smoothed), uphill
    /// end first, for grind.exe runs (SK_RAILS="world min_metres").
    #[test]
    fn long_rails() {
        let Ok(spec) = std::env::var("SK_RAILS") else { return };
        let w: Vec<&str> = spec.split_whitespace().collect();
        let Some(mut tris) = load(w[0]) else { return };
        let min: f32 = w.get(1).and_then(|v| v.parse().ok()).unwrap_or(12.);
        let mut report = Report::default();
        weld(&mut tris, 1.0, &mut report);
        drop_copies(&mut tris, 1.0);
        for mut r in smooth_rails(rails_of(&tris), 1.0, &mut report) {
            let length: f32 = r.windows(2).map(|s| s[0].distance(s[1])).sum();
            if length < min {
                continue;
            }
            if r[0].z < r[r.len() - 1].z {
                r.reverse();
            }
            let max_turn = r.windows(3).map(|s| {
                (s[1] - s[0]).normalize_or_zero().dot((s[2] - s[1]).normalize_or_zero()).clamp(-1., 1.).acos().to_degrees()
            }).fold(0f32, f32::max);
            // Where to drop on: 1.5 m in, heading along the first 3 m.
            let mut ahead = r[r.len() - 1];
            let mut walked = 0.;
            let mut start = r[0];
            for s in r.windows(2) {
                let d = s[0].distance(s[1]);
                if walked + d >= 1.5 && walked <= 1.5 {
                    start = s[0].lerp(s[1], (1.5 - walked) / d);
                }
                if walked + d >= 4.5 {
                    ahead = s[0].lerp(s[1], (4.5 - walked) / d);
                    break;
                }
                walked += d;
            }
            let dir = (ahead - start).normalize_or_zero();
            println!("RAIL {:.2} {:.2} {:.2} yaw {:.4} pitch {:.4} length {length:.1} drop {:.2} turn {max_turn:.1} points {}",
                start.x, start.y, start.z, dir.y.atan2(dir.x), dir.z.asin(), r[0].z - r[r.len() - 1].z, r.len());
        }
    }

    /// Rails crossing a box, before and after smoothing, joint by joint
    /// (SK_BOX="x0 y0 x1 y1 world" in the environment).
    #[test]
    fn rails_in_a_box() {
        let Ok(spec) = std::env::var("SK_BOX") else { return };
        let w: Vec<&str> = spec.split_whitespace().collect();
        let (x0, y0, x1, y1): (f32, f32, f32, f32) = (w[0].parse().unwrap(), w[1].parse().unwrap(), w[2].parse().unwrap(), w[3].parse().unwrap());
        let Some(tris) = load(w[4]) else { return };
        let inside = |p: &Vec3| p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
        let show = |label: &str, rails: &[Vec<Vec3>]| {
            for (n, r) in rails.iter().enumerate().filter(|(_, r)| r.iter().any(|p| inside(p))) {
                let length: f32 = r.windows(2).map(|s| s[0].distance(s[1])).sum();
                if length < 3. {
                    continue;
                }
                println!("{label} rail {n}: {} points, {length:.1} m", r.len());
                for (k, p) in r.iter().enumerate() {
                    let turn = if k > 0 && k + 1 < r.len() {
                        let (a, b) = ((r[k] - r[k - 1]).normalize_or_zero(), (r[k + 1] - r[k]).normalize_or_zero());
                        format!(" turn {:.1} deg (pitch {:+.1})", a.dot(b).clamp(-1., 1.).acos().to_degrees(),
                            (b.z.asin() - a.z.asin()).to_degrees())
                    } else {
                        String::new()
                    };
                    println!("   {:.3} {:.3} {:.3}{turn}", p.x, p.y, p.z);
                }
            }
        };
        let before = rails_of(&tris);
        show("before", &before);
        let mut welded = tris.clone();
        let mut report = Report::default();
        weld(&mut welded, 1.0, &mut report);
        let after = smooth_rails(rails_of(&welded), 1.0, &mut report);
        show("after", &after);
    }

    #[test]
    fn a_wobbly_downhill_rail_comes_out_straight_with_a_rounded_dip() {
        // 8 m down at 30 degrees with 1 cm wobble, then 3 m flat.
        let mut rail = Vec::new();
        for k in 0..=16 {
            let x = k as f32 * 0.5;
            rail.push(Vec3::new(x, 0.01 * ((k % 2) as f32 * 2. - 1.), 4.6 - x * 0.577));
        }
        rail.push(Vec3::new(11., 0., 4.6 - 8. * 0.577));
        let mut report = Report::default();
        let out = smooth_rails(vec![rail], 1.0, &mut report);
        let r = roughness(&[], &out);
        assert_eq!(out.len(), 1);
        assert!(r.sharpest < 6., "sharpest joint {}", r.sharpest);
        assert!(report.rail_max_move <= RAIL_MAX_MOVE + 1e-3);
    }

    #[test]
    fn a_rail_broken_by_a_small_gap_is_joined() {
        let a = vec![Vec3::new(0., 0., 1.), Vec3::new(2., 0., 1.)];
        let b = vec![Vec3::new(2.05, 0.01, 1.), Vec3::new(4., 0., 1.)];
        let c = vec![Vec3::new(2.05, 1., 1.), Vec3::new(2.05, 3., 1.)]; // across: stays apart
        let mut report = Report::default();
        let out = smooth_rails(vec![a, b, c], 1.0, &mut report);
        assert_eq!(out.len(), 2);
        assert_eq!(report.rails_joined, 1);
    }

    #[test]
    fn a_seam_step_between_two_slabs_is_closed_but_a_curb_stays() {
        // Two road slabs meeting at x = 1 with a 6 mm step; a 12 cm curb at x = 2.
        let q = |x0: f32, x1: f32, z0: f32, z1: f32| -> [[Vec3; 3]; 2] {
            [
                [Vec3::new(x0, 0., z0), Vec3::new(x1, 0., z1), Vec3::new(x1, 1., z1)],
                [Vec3::new(x0, 0., z0), Vec3::new(x1, 1., z1), Vec3::new(x0, 1., z0)],
            ]
        };
        let mut tris: Vec<[Vec3; 3]> = Vec::new();
        tris.extend(q(0., 1., 0., 0.));
        tris.extend(q(1., 2., 0.006, 0.006));
        tris.extend(q(2., 3., 0.126, 0.126));
        let before = roughness(&tris, &[]);
        let mut report = Report::default();
        weld(&mut tris, 1.0, &mut report);
        let after = roughness(&tris, &[]);
        assert!(before.steps_5mm >= 1);
        assert_eq!(after.steps_5mm, 0, "{after:?}");
        assert!(tris.iter().flatten().any(|v| (v.z - 0.126).abs() < 1e-4), "the curb stays");
    }
}
