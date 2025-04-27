# TypeDowncaster Documentation

## Overview

TypeDowncaster is an LLVM optimization pass designed to automatically identify and downcast unnecessarily large data types in program code. It helps reduce memory footprint through strategic downcasting of larger data types (such as 64-bit integers to 32-bit integers and double-precision floating-point values to single-precision) when it is safe to do so.

## Purpose and Benefits

- **Memory Usage Reduction**: Automatically identifies and transforms unnecessarily large data types to smaller alternatives
- **Performance Improvement**: Potential performance gains from improved cache utilization
- **Minimal Effort**: No manual code changes required by developers
- **Safety Guaranteed**: Uses sophisticated static analysis to ensure downcasting is safe

## Detailed Features

### Type Optimization Capabilities

1. **Integer Downcasting**
   - Converts `i64` (64-bit) integers to `i32` (32-bit) integers when value ranges allow
   - Uses LLVM's ScalarEvolution framework to analyze value ranges
   - Only performs transformations when statically proven to be safe

2. **Floating-Point Optimization**
   - Converts `double` (64-bit) to `float` (32-bit) when precision loss is acceptable
   - Analyzes precision requirements for constants and operations

3. **Composite Type Handling**
   - Optimizes arrays containing larger types
   - Optimizes vector types
   - Transforms individual fields within structures

4. **Memory Allocation Optimization**
   - Optimizes stack allocations (`alloca` instructions)
   - Transforms global variables
   - Maintains all necessary attributes (alignment, volatility, etc.)

## Technical Implementation

### Analysis Components

- **Type Eligibility Analysis**: Identifies types that could potentially be downsized
- **Value Range Analysis**: Determines if values will fit in smaller types
- **Use Identification**: Tracks all uses of transformed allocations
- **Safe Transformation**: Inserts proper casts to maintain program semantics

### Safety Mechanisms

TypeDowncaster employs multiple safety checks to guarantee program correctness:

- **Static Range Analysis**: Uses ScalarEvolution to compute possible value ranges
- **Conservative Approach**: Only transforms when safety can be proven
- **Proper Cast Insertion**: Automatically inserts necessary casts for type conversion
- **Use Verification**: Ensures all uses are properly transformed

## Integration with LLVM

### Building TypeDowncaster

```bash
# Navigate to TypeDowncaster directory
cd TypeDowncaster

# Create build directory
mkdir build && cd build

# Configure with CMake
cmake -DLLVM_DIR=/path/to/llvm/build ..

# Build the plugin
make
```

### Using with LLVM Tools

#### Running with opt Tool

```bash
# Basic usage
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes=type-downcaster input.ll -o output.ll

# With verbose debugging output
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes=type-downcaster -debug-only=typedowncaster input.ll -o output.ll

# With statistics collection
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes=type-downcaster -stats input.ll -o output.ll
```

#### Using in a Pipeline

TypeDowncaster can be included in a larger optimization pipeline:

```bash
# At the end of the standard optimization pipeline
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes='default<O2>,type-downcaster' input.ll -o output.ll
```

## Performance Metrics and Statistics

TypeDowncaster collects the following statistics during optimization:

- `NumAllocasOptimized`: Number of stack allocations optimized
- `NumGlobalsOptimized`: Number of global variables optimized
- `NumStructFieldsOptimized`: Number of struct fields optimized
- `NumFloatToFloatOptimized`: Number of double to float conversions
- `NumTotalBytesReduced`: Total bytes saved across all allocations

View these statistics by adding the `-stats` flag when running opt.

## Example Transformations

### Integer Downcasting Example

**Before:**
```llvm
@global_var = global i64 42

define void @example() {
  %local_var = alloca i64
  store i64 100, i64* %local_var
  %val = load i64, i64* %local_var
  ret void
}
```

**After:**
```llvm
@global_var.optimized = global i32 42

define void @example() {
  %local_var.optimized = alloca i32
  store i32 100, i32* %local_var.optimized
  %val.downcasted = load i32, i32* %local_var.optimized
  %val = sext i32 %val.downcasted to i64
  ret void
}
```

### Floating-Point Precision Example

**Before:**
```llvm
define void @example() {
  %x = alloca double
  store double 3.14159, double* %x
  %val = load double, double* %x
  ret void
}
```

**After:**
```llvm
define void @example() {
  %x.optimized = alloca float
  store float 3.14159, float* %x.optimized
  %val.downcasted = load float, float* %x.optimized
  %val = fpext float %val.downcasted to double
  ret void
}
```

## Troubleshooting

### Common Issues

1. **No Optimization Performed**
   - Ensure that the types in your code can be safely downcasted
   - Check that you're using the correct pass name (`type-downcaster`)

2. **Compilation Errors After Optimization**
   - This could indicate a bug in TypeDowncaster
   - Please report with minimal reproducible example

### Debugging

For detailed debugging information, add the following flags:
```bash
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes=type-downcaster -debug-only=typedowncaster -debug-verbose input.ll -o output.ll
```

## Limitations

- Only handles specific type transformations (i64→i32, double→float)
- Conservative analysis may miss some safe optimization opportunities
- Does not optimize across function boundaries (interprocedural)
- Not suitable for programs that genuinely require full 64-bit precision

## Future Directions

Planned enhancements to TypeDowncaster include:

- Support for additional type transformations (i32→i16, i16→i8)
- Profile-guided optimization to make more informed decisions
- Interprocedural analysis for cross-function optimization
- Machine learning-based optimization heuristics
- Auto-tuning of precision requirements based on correctness criteria

## Contact and Support

For questions, issues, or contributions, please use the project's GitHub issue tracker or contact the development team at [email address].

## License

TypeDowncaster is licensed under the Apache License 2.0 with LLVM Exceptions.
