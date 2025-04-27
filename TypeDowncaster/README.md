# TypeDowncaster

An LLVM optimization pass that automatically downcasts unnecessarily large data types (such as 64-bit integers to 32-bit integers and double-precision floating-point values to single-precision) when it is safe to do so.

## Building TypeDowncaster

```bash
# Create build directory
mkdir build && cd build

# Configure with CMake (replace with your LLVM build directory)
cmake -DLLVM_DIR=/path/to/llvm/build ..

# Build the plugin
make
```

## Using TypeDowncaster

```bash
# Basic usage
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes=type-downcaster input.ll -o output.ll

# With statistics
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes=type-downcaster -stats input.ll -o output.ll

# With debug output
opt -load-pass-plugin=./lib/TypeDowncaster.so -passes=type-downcaster -debug-only=typedowncaster input.ll -o output.ll
```

## Features

- Downcasts 64-bit integers to 32-bit integers when safe
- Converts double-precision floats to single-precision where precision loss is acceptable
- Optimizes stack allocations, global variables, and struct fields
- Uses LLVM's ScalarEvolution framework to ensure safety

For complete documentation, see the [Documentation.md](Documentation.md) file.
