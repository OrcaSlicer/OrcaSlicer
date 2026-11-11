use crate::quantify::tracker::CTEntry;
use std::ops::{Index, IndexMut};

// triangular matrix of pair-wise collision loss and weights
// supporting data structure for the `CollisionTracker`
#[derive(Debug, Clone)]
pub struct PairMatrix {
    pub size: usize,
    pub data: Vec<CTEntry>,
}

impl PairMatrix {
    pub fn new(size: usize) -> Self {
        let len = size * (size + 1) / 2;
        Self {
            size,
            data: vec![CTEntry { weight: 1.0, loss: 0.0 }; len],
        }
    }
}

impl Index<(usize, usize)> for PairMatrix {
    type Output = CTEntry;

    fn index(&self, (row, col): (usize, usize)) -> &Self::Output {
        &self.data[calc_idx(row, col, self.size)]
    }
}

impl IndexMut<(usize, usize)> for PairMatrix {
    fn index_mut(&mut self, (row, col): (usize, usize)) -> &mut Self::Output {
        &mut self.data[calc_idx(row, col, self.size)]
    }
}

fn calc_idx(row: usize, col: usize, size: usize) -> usize {
    /* Example:
        0 1 2 3
          4 5 6
            7 8
              9
    */
    debug_assert!(row < size && col < size);
    if row <= col {
        (row * size) + col - ((row * (row + 1)) / 2)
    } else {
        (col * size) + row - ((col * (col + 1)) / 2)
    }
}
