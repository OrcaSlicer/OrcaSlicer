# Support

Support structures are used in 3D printing to provide stability to overhangs and complex geometries.

- [Type](#type)
  - [Normal](#normal)
  - [Tree](#tree)
    - [Support critical regions only](#support-critical-regions-only)
  - [Auto](#auto)
  - [Manual](#manual)
- [Style](#style)
  - [Grid](#grid)
  - [Snug](#snug)
  - [Organic](#organic)
  - [Tree Slim](#tree-slim)
  - [Tree Strong](#tree-strong)
  - [Tree Hybrid](#tree-hybrid)
- [Threshold angle](#threshold-angle)
- [Threshold overlap](#threshold-overlap)
- [Initial layer density](#initial-layer-density)
- [Initial layer expansion](#initial-layer-expansion)
- [On build plate only](#on-build-plate-only)
- [Ignore small overhangs](#ignore-small-overhangs)

## Type

Support structures can be generated in various styles, each suited for different printing needs:

### Normal

Normal support structures are generated in a grid pattern, providing a stable base for overhangs and complex geometries.

### Tree

Tree-like support structures are designed to minimize material usage while still providing adequate support. They branch out from a central trunk, allowing for more efficient printing.

#### Support critical regions only

Only create support for critical regions including sharp tail, cantilever, etc.

### Auto

Automatically generates support structures where needed, based on the model's geometry and overhangs and manual placement in the prepare view.

### Manual

Limit support generation to specific areas defined by manual placement in the prepare view.

## Style

Style and shape of the support.  

### Grid

Default in normal support, projecting the supports into a regular grid will create more stable supports.

### Snug

Snug support towers will save material and reduce object scarring.

### Organic

Default style for tree support, which merges slim and organic style branches more aggressively and saves material.

### Tree Slim

Slim tree support branches are designed to be more delicate and use less material while still providing adequate support.

### Tree Strong

Strong tree support branches are designed to be more robust and provide additional support for heavier overhangs.

### Tree Hybrid

Create similar structure to normal support under large flat overhangs.

## Threshold angle

Support will be generated for overhangs whose slope angle is below the threshold.

## Threshold overlap

If threshold angle is zero, support will be generated for overhangs whose overlap is below the threshold.
The smaller this value is, the steeper the overhang that can be printed without support.

## Initial layer density

Density of the first raft or support layer.

## Initial layer expansion

Expand the first raft or support layer to improve bed plate adhesion.

## On build plate only

Don't create support on model surface, only on build plate.

## Ignore small overhangs

With this setting small overhangs that possibly need no supports will be ignored from support generation.
