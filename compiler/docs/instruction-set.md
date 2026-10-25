# Instruction Set (DRAFT)

A small instruction set for the machine this toolchain targets. Everything here is a first guess and will change as the emulator and compiler get built.

## Machine model

- 8 general-purpose registers: `R0`–`R7`, 16 bits each
- 16-bit program counter
- 16-bit instructions, fixed width
- Separate halt state; `HALT` stops the machine

## Instructions

| Mnemonic | Operands | Meaning |
|----------|----------|---------|
| `NOP` | | Do nothing |
| `LDI` | `Rd, imm8` | Load an 8-bit constant into `Rd` |
| `MOV` | `Rd, Rs` | Copy `Rs` into `Rd` |
| `ADD` | `Rd, Rs` | `Rd = Rd + Rs` |
| `SUB` | `Rd, Rs` | `Rd = Rd - Rs` |
| `MUL` | `Rd, Rs` | `Rd = Rd * Rs` |
| `JMP` | `addr` | Jump to `addr` |
| `JZ` | `Rs, addr` | Jump to `addr` if `Rs == 0` |
| `PRINT` | `Rs` | Output the value of `Rs` |
| `HALT` | | Stop the machine |

## Open questions

- Instruction encoding (opcode bits vs. operand bits)
- How to handle memory loads and stores
- Whether to add a call/return mechanism or keep it flat
