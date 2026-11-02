use crate::consts::{GLS_WEIGHT_DECAY, GLS_WEIGHT_MAX_INC_RATIO, GLS_WEIGHT_MIN_INC_RATIO};
use crate::quantify::pair_matrix::PairMatrix;
use crate::quantify::{quantify_collision_poly_container, quantify_collision_poly_poly};
use crate::util::assertions::tracker_matches_layout;
use jagua_rs::collision_detection::hazards::collector::{BasicHazardCollector, HazardCollector};
use jagua_rs::collision_detection::hazards::HazardEntity;
use jagua_rs::entities::{Layout, PItemKey};
use ordered_float::Float;
use slotmap::SecondaryMap;

/// Tracker of both collisions between pair of items and collisions with the container.
/// It also stores the weights for every pair of hazards and is used as a cache for collisions.
#[derive(Debug, Clone)]
pub struct CollisionTracker {
    pub size: usize,
    pub pk_idx_map: SecondaryMap<PItemKey, usize>,
    pub pair_collisions: PairMatrix,
    pub container_collisions: Vec<CTEntry>,
    // SPARROW_ARRANGE PATCH: items whose `item_id >= n_movable` are pinned obstacles (bed holes and
    /// caller-fixed items, modelled as immovable placed items). They contribute loss to the
    /// movable items that overlap them, but carry none of their own, which both keeps them
    /// out of `move_items`' candidate list (it selects on `get_loss(pk) > 0.0`) and stops
    /// obstacle-vs-obstacle or obstacle-vs-container overlap from making the layout look
    /// permanently infeasible.
    pub pinned: Vec<bool>,
}

pub type CTSnapshot = CollisionTracker;

impl CollisionTracker {
    // SPARROW_ARRANGE PATCH: `n_movable` added; ids at or above it are pinned obstacles.
    pub fn new(l: &Layout, n_movable: usize) -> Self {
        let size = l.placed_items.len();

        let pk_idx_map: SecondaryMap<PItemKey, usize> = l.placed_items.keys().enumerate()
            .map(|(i, pk)| (pk, i))
            .collect();

        let mut pinned = vec![false; size];
        for (pk, pi) in l.placed_items.iter() {
            pinned[pk_idx_map[pk]] = pi.item_id >= n_movable;
        }

        // Create the tracker
        let mut ot = Self {
            size,
            pk_idx_map,
            pair_collisions: PairMatrix::new(size),
            container_collisions: vec![CTEntry { weight: 1.0, loss: 0.0 }; size],
            pinned,
        };

        // Recompute the loss for all items
        l.placed_items.keys().for_each(|pk| {
            ot.recompute_loss_for_item(pk, l)
        });

        debug_assert!(tracker_matches_layout(&ot, l));

        ot
    }

    /// Returns true if this item is a pinned obstacle. // SPARROW_ARRANGE PATCH
    pub fn is_pinned(&self, pk: PItemKey) -> bool {
        self.pinned[self.pk_idx_map[pk]]
    }

    fn recompute_loss_for_item(&mut self, pk: PItemKey, l: &Layout) {
        let idx = self.pk_idx_map[pk];
        // SPARROW_ARRANGE PATCH: leave a pinned obstacle's entries at zero. Returning before the reset below is
        // deliberate: the obstacle-vs-movable entries are shared, and are written when the
        // *movable* side is recomputed.
        if self.pinned[idx] {
            return;
        }
        let pi = &l.placed_items[pk];
        let shape = &pi.shape;

        // Reset all current loss values for the item
        for i in 0..self.size {
            self.pair_collisions[(idx, i)].loss = 0.0;
        }
        self.container_collisions[idx].loss = 0.0;

        // Compute which hazards are currently colliding with the item
        let mut collector = BasicHazardCollector::with_capacity(l.placed_items.len() + 1);
        l.cde().collect_poly_collisions(shape, &mut collector);
        // Remove the item itself from the detector
        collector.remove_by_entity(&HazardEntity::from((pk, pi)));

        // For each colliding hazard, quantify the collision and store it in the tracker
        for (_, haz) in collector.iter() {
            match haz {
                HazardEntity::PlacedItem { pk: other_pk, .. } => {
                    let shape_other = &l.placed_items[*other_pk].shape;
                    let idx_other = self.pk_idx_map[*other_pk];

                    let loss = quantify_collision_poly_poly(shape, shape_other);
                    assert!(loss > 0.0, "loss for a collision should be > 0.0");
                    self.pair_collisions[(idx, idx_other)].loss = loss;
                }
                HazardEntity::Exterior => {
                    let loss = quantify_collision_poly_container(shape, l.container.outer_cd.bbox);
                    assert!(loss > 0.0, "loss for a collision should be > 0.0");
                    self.container_collisions[idx].loss = loss;
                }
                _ => unimplemented!("unsupported hazard entity"),
            }
        }
    }

    pub fn restore_but_keep_weights(&mut self, cts: &CTSnapshot, layout: &Layout) {
        //Copy the loss and keys, but keep the weights
        self.pk_idx_map = cts.pk_idx_map.clone();
        self.pinned = cts.pinned.clone(); // SPARROW_ARRANGE PATCH
        self.pair_collisions.data.iter_mut()
            .zip(cts.pair_collisions.data.iter())
            .for_each(|(a, b)| a.loss = b.loss);
        self.container_collisions.iter_mut()
            .zip(cts.container_collisions.iter())
            .for_each(|(a, b)| a.loss = b.loss);
        debug_assert!(tracker_matches_layout(self, layout));
    }

    pub fn save(&self) -> CTSnapshot {
        self.clone()
    }

    pub fn register_item_move(&mut self, l: &Layout, old_pk: PItemKey, new_pk: PItemKey) {
        //swap the keys in the pk_idx_map
        let idx = self.pk_idx_map.remove(old_pk).unwrap();
        self.pk_idx_map.insert(new_pk, idx);

        self.recompute_loss_for_item(new_pk, l);

        debug_assert!(tracker_matches_layout(self, l));
    }


    /// Algorithm 8 from https://doi.org/10.48550/arXiv.2509.13329
    pub fn update_weights(&mut self) {
        // Find the maximum loss across all entries
        let max_loss = self.pair_collisions.data.iter()
            .chain(self.container_collisions.iter())
            .map(|e| e.loss)
            .fold(0.0, |a, b| a.max(b));

        // Go over all entries (pairs) and modify their weights.
        for e in self.pair_collisions.data.iter_mut()
            .chain(self.container_collisions.iter_mut()) {
            let multiplier = match e.loss == 0.0 {
                true => {
                    // No collision at the moment, slowly decay the weight back to 1.0
                    GLS_WEIGHT_DECAY
                },
                false => {
                    // Collision detected, increase the weight based on 'how bad' the collision is relative to the worst collision
                    GLS_WEIGHT_MIN_INC_RATIO + (GLS_WEIGHT_MAX_INC_RATIO - GLS_WEIGHT_MIN_INC_RATIO) * (e.loss / max_loss)
                },
            };
            e.weight = (e.weight * multiplier).max(1.0);
        }
    }

    pub fn get_pair_weight(&self, pk1: PItemKey, pk2: PItemKey) -> f32 {
        let (idx1, idx2) = (self.pk_idx_map[pk1], self.pk_idx_map[pk2]);
        self.pair_collisions[(idx1, idx2)].weight
    }

    pub fn get_container_weight(&self, pk: PItemKey) -> f32 {
        let idx = self.pk_idx_map[pk];
        self.container_collisions[idx].weight
    }

    /// Algorithm 1 from https://doi.org/10.48550/arXiv.2509.13329
    /// Evaluations between item pairs are stored in this data-structure for quick and easy retrieval.
    pub fn get_pair_loss(&self, pk1: PItemKey, pk2: PItemKey) -> f32 {
        let (idx1, idx2) = (self.pk_idx_map[pk1], self.pk_idx_map[pk2]);
        self.pair_collisions[(idx1, idx2)].loss
    }

    pub fn get_container_loss(&self, pk: PItemKey) -> f32 {
        let idx = self.pk_idx_map[pk];
        self.container_collisions[idx].loss
    }

    pub fn get_loss(&self, pk: PItemKey) -> f32 {
        let idx = self.pk_idx_map[pk];
        // SPARROW_ARRANGE PATCH: pinned obstacles never report loss, so they are never picked to move.
        if self.pinned[idx] {
            return 0.0;
        }

        let pair_loss = (0..self.size)
            .map(|i| self.pair_collisions[(idx, i)].loss)
            .sum::<f32>();

        self.container_collisions[idx].loss + pair_loss
    }

    pub fn get_weighted_loss(&self, pk: PItemKey) -> f32 {
        let idx = self.pk_idx_map[pk];
        // SPARROW_ARRANGE PATCH
        if self.pinned[idx] {
            return 0.0;
        }

        let w_pair_loss = (0..self.size)
            .map(|i| self.pair_collisions[(idx, i)].weighted_loss())
            .sum::<f32>();

        self.container_collisions[idx].weighted_loss() + w_pair_loss
    }

    pub fn get_total_loss(&self) -> f32 {
        let cont_o = self.container_collisions.iter().map(|e| e.loss).sum::<f32>();

        let pair_o = self.pair_collisions.data.iter()
            .map(|e| e.loss)
            .sum::<f32>();

        cont_o + pair_o
    }

    pub fn get_total_weighted_loss(&self) -> f32 {
        let cont_w_o = self.container_collisions.iter()
            .map(|e| e.weighted_loss())
            .sum::<f32>();

        let pair_w_o = self.pair_collisions.data.iter()
            .map(|e| e.weighted_loss())
            .sum::<f32>();

        cont_w_o + pair_w_o
    }
}

#[derive(Debug, Clone, Copy)]
pub struct CTEntry {
    pub loss: f32,
    pub weight: f32,
}

impl CTEntry {
    pub fn weighted_loss(&self) -> f32 {
        self.weight * self.loss
    }
}