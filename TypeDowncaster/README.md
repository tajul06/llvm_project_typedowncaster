# TypeDowncaster - An LLVM Optimization Pass for Data Type Downcasting

TypeDowncaster is a standalone LLVM optimization pass designed to identify and downcast data types in your code when it is safe to do so. This pass aims to reduce memory usage and potentially improve performance by using smaller data types where larger ones are not necessary.

## Features

- **Integer Downcasting**: Converts 64-bit integers (`i64`) to 32-bit integers (`i32`) when the value range allows
- **Floating-point Precision Reduction**: Converts `double` to `float` when precision loss is acceptable
- **Composite Type Optimization**: Optimizes arrays, vectors, and struct fields containing overly-large types
- **Memory Allocation Optimization**:
  - Rewrites stack-allocated variables (`alloca` instructions) 
  - Rewrites global variables
  - Preserves all attributes like alignment, volatility, etc.
- **Safe Transformations**: Uses LLVM's ScalarEvolution for range analysis to ensure transformations are safe

## Build Instructions

### Prerequisites

- CMake (3.13.4 or newer)
- LLVM (same version as your target)
- C++ compiler with C++17 support

### Building

```bash
# Clone this repository
git clone https://github.com/yourusername/TypeDowncaster.git
cd TypeDowncaster

# Create build directory
mkdir build && cd build

# Configure with CMake
# You may need to set LLVM_DIR to point to your LLVM build or installation
cmake -DLLVM_DIR=/path/to/llvm/build ..

# Build the project
make
```

## Usage

### As an LLVM opt Tool Plugin

```bash
# Run with opt tool (from LLVM)
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes=type-downcaster input.ll -o output.ll

# Add verbose output for debugging
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes=type-downcaster -debug-only=typedowncaster input.ll -o output.ll
```

### Integrate with Other Passes

You can also include the TypeDowncaster pass in a larger optimization pipeline:

```bash
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes='default<O2>,type-downcaster' input.ll -o output.ll
```

## Example

### Before Optimization

```llvm
define void @example() {
  %ptr = alloca i64
  store i64 42, i64* %ptr
  %val = load i64, i64* %ptr
  ret void
}
```

### After Optimization

```llvm
define void @example() {
  %ptr.optimized = alloca i32
  store i32 42, i32* %ptr.optimized
  %val.downcasted = load i32, i32* %ptr.optimized
  %val = sext i32 %val.downcasted to i64
  ret void
}
```

## Statistics

The pass collects the following statistics that can be displayed using `-stats`:

- `NumAllocasOptimized`: Number of stack allocations optimized
- `NumGlobalsOptimized`: Number of global variables optimized
- `NumStructFieldsOptimized`: Number of struct fields optimized
- `NumFloatToFloatOptimized`: Number of double to float conversions
- `NumTotalBytesReduced`: Total bytes saved in memory allocations

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

This project is licensed under the Apache License 2.0 with LLVM Exceptions - see the LICENSE file for details.
