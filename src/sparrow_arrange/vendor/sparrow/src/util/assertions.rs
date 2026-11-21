use crate::quantify::tracker::CollisionTracker;
use crate::quantify::{quantify_collision_poly_container, quantify_collision_poly_poly};
use float_cmp::{approx_eq, assert_approx_eq};
use itertools::Itertools;
use jagua_rs::collision_detection::hazards::collector::{BasicHazardCollector, HazardCollector};
use jagua_rs::collision_detection::hazards::HazardEntity;
use jagua_rs::entities::Layout;
use jagua_rs::util::assertions;
use log::warn;

pub fn tracker_matches_layout(ct: &CollisionTracker, l: &Layout) -> bool {
    assert!(l.placed_items.keys().all(|k| ct.pk_idx_map.contains_key(k)));
    assert!(assertions::layout_qt_matches_fresh_qt(l));

    for (pk1, pi1) in l.placed_items.iter() {
        // SPARROW_ARRANGE PATCH: pinned obstacles deliberately carry no loss of their own, so the
        // invariants below do not apply to them.
        if ct.is_pinned(pk1) {
            continue;
        }
        let mut collector = BasicHazardCollector::new();
        l.cde().collect_poly_collisions(&pi1.shape, &mut collector);
        collector.remove_by_entity(&HazardEntity::from((pk1, pi1)));
        assert_eq!(ct.get_pair_loss(pk1, pk1), 0.0);
        for (pk2, pi2) in l.placed_items.iter().filter(|(k, _)| *k != pk1) {
            let stored_loss = ct.get_pair_loss(pk1, pk2);
            match collector.iter().any(|(_, he)| he == &HazardEntity::from((pk2, pi2))) {
                true => {
                    let calc_loss = quantify_collision_poly_poly(&pi1.shape, &pi2.shape);
                    let calc_loss_r = quantify_collision_poly_poly(&pi2.shape, &pi1.shape);
                    if !approx_eq!(f32,calc_loss,stored_loss,epsilon = 0.10 * stored_loss) && !approx_eq!(f32,calc_loss_r,stored_loss, epsilon = 0.10 * stored_loss) {
                        let mut opp_collector = BasicHazardCollector::new();
                        l.cde().collect_poly_collisions(&pi2.shape, &mut opp_collector);
                        opp_collector.remove_by_entity(&HazardEntity::from((pk2, pi2)));
                        if opp_collector.contains_entity(&((pk1, pi1).into())) {
                            dbg!(&pi1.shape.vertices, &pi2.shape.vertices);
                            dbg!(
                                stored_loss,
                                calc_loss,
                                calc_loss_r,
                                opp_collector.iter().collect_vec(),
                                HazardEntity::from((pk1, pi1)),
                                HazardEntity::from((pk2, pi2))
                            );
                            panic!("tracker error");
                        }  else if stored_loss == 0.0 {
                            //detecting collisions is not symmetrical (in edge cases)
                            warn!("non-symmetrical collision!");
                            // dbg!(stored_loss, calc_loss, calc_loss_r);
                            // warn!(
                            //         "collisions: pi_1 {:?} -> {:?}",
                            //         HazardEntity::from((pk1, pi1)),
                            //         collector.iter().collect_vec()
                            //     );
                            // warn!(
                            //         "opposite collisions: pi_2 {:?} -> {:?}",
                            //         HazardEntity::from((pk2, pi2)),
                            //         opp_collector.iter().collect_vec()
                            //     );
                            //
                            // warn!(
                            //         "pi_1: {:?}",
                            //         pi1.shape
                            //             .vertices
                            //             .iter()
                            //             .map(|p| format!("({},{})", p.0, p.1))
                            //             .collect_vec()
                            //     );
                            // warn!(
                            //         "pi_2: {:?}",
                            //         pi2.shape
                            //             .vertices
                            //             .iter()
                            //             .map(|p| format!("({},{})", p.0, p.1))
                            //             .collect_vec()
                            //     );
                        }
                        else {
                            dbg!(&pi1.shape.vertices, &pi2.shape.vertices);
                            dbg!(
                                    stored_loss,
                                    calc_loss,
                                    calc_loss_r,
                                    opp_collector.iter().collect_vec(),
                                    HazardEntity::from((pk1, pi1)),
                                    HazardEntity::from((pk2, pi2))
                                );
                            panic!("tracker error");
                        }
                    }
                }
                false => {
                    if stored_loss != 0.0 {
                        let calc_loss = quantify_collision_poly_poly(&pi1.shape, &pi2.shape);
                        let mut opp_collector = BasicHazardCollector::new();
                        l.cde().collect_poly_collisions(&pi2.shape, &mut opp_collector);
                        opp_collector.remove_by_entity(&HazardEntity::from((pk2, pi2)));
                        if !opp_collector.contains_entity(&HazardEntity::from((pk1, pi1))) {
                            dbg!(&pi1.shape.vertices, &pi2.shape.vertices);
                            dbg!(
                                stored_loss,
                                calc_loss,
                                opp_collector.iter().collect_vec(),
                                HazardEntity::from((pk1, pi1)),
                                HazardEntity::from((pk2, pi2))
                            );
                            panic!("tracker error");
                        } else {
                            //detecting collisions is not symmetrical (in edge cases)
                            warn!("inconsistent loss");
                            warn!(
                                "collisions: {:?} -> {:?}",
                                HazardEntity::from((pk1, pi1)),
                                collector.iter().collect_vec()
                            );
                            warn!(
                                "opposite collisions: {:?} -> {:?}",
                                HazardEntity::from((pk2, pi2)),
                                opp_collector.iter().collect_vec()
                            );
                        }
                    }
                }
            }
        }
        if collector.contains_entity(&HazardEntity::Exterior) {
            let stored_loss = ct.get_container_loss(pk1);
            let calc_loss = quantify_collision_poly_container(&pi1.shape, l.container.outer_cd.bbox);
            assert_approx_eq!(f32, stored_loss, calc_loss, ulps = 5);
        } else {
            assert_eq!(ct.get_container_loss(pk1), 0.0);
        }
    }

    true
}
