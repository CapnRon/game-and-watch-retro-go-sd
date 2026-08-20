#ifndef _GW_FLASH_H_
#define _GW_FLASH_H_

#include "stm32h7xx_hal.h"

#include <stdint.h>
#include <stdbool.h>

void OSPI_EnableMemoryMappedMode(void);
void OSPI_DisableMemoryMappedMode(void);
void OSPI_ChipErase(void);

// Performs one erase command per call with the largest size possible.
// Sets *address and *size to values that should be passed to
// OSPI_Erase in the next iteration.
// Returns true when done.
bool OSPI_Erase(uint32_t *address, uint32_t *size, bool blocking);

// Erases the area synchronously. Will block until it's done.
void OSPI_EraseSync(uint32_t address, uint32_t size);

void OSPI_PageProgram(uint32_t address, const uint8_t *buffer, size_t buffer_size);
void OSPI_NOR_WriteEnable(void);
void OSPI_Program(uint32_t address, const uint8_t *buffer, size_t buffer_size);

/* Indirect read with the configured read command (PSRAM: 0xEB quad). */
void OSPI_IndirectRead(uint32_t address, uint8_t *data, size_t len);

/* Indirect write with the configured write command (PSRAM: 02h page write).
 * One command per call — the XSPI holds CS low for the whole transfer. */
void OSPI_IndirectWrite(uint32_t address, const uint8_t *data, size_t len);
void OSPI_PsramReadStatus(uint8_t *out);

void OSPI_ReadJedecId(uint8_t dest[3]);
void OSPI_ReadSR(uint8_t dest[1]);
void OSPI_ReadCR(uint8_t dest[1]);
const char* OSPI_GetFlashName(void);
uint32_t OSPI_GetSmallestEraseSize(void);
uint32_t OSPI_GetFlashSize(void);

void OSPI_Init(OSPI_HandleTypeDef *hospi);

#endif
