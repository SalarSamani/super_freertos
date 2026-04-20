#ifndef FREERTOS_RISC_V_CHIP_SPECIFIC_EXTENSIONS_H
#define FREERTOS_RISC_V_CHIP_SPECIFIC_EXTENSIONS_H

/* No additional per-task CSRs on super_ibex step 1. When you add FPU or
 * per-task PMP in future steps, save/restore them in portASM.S via
 * macros defined here.                                                  */

#define portasmADDITIONAL_CONTEXT_SIZE   0
#define portasmSAVE_ADDITIONAL_REGISTERS
#define portasmRESTORE_ADDITIONAL_REGISTERS

#endif