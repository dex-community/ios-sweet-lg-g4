// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Integer types for the eMMC driver.
//
// The core is compiled twice: into the kernel from the XNU headers, and on a
// build host for the unit tests. Kernel builds do not guarantee a hosted
// <stdint.h>, so the widths are spelled out here instead of pulled from a
// library header.
#ifndef NATIVE_SDHCI_TYPES_H
#define NATIVE_SDHCI_TYPES_H

namespace sdhci {

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

// The driver assumes these widths; make a wrong target a compile error rather
// than a silent misread of a register.
static_assert(sizeof(u8) == 1, "u8 must be one byte");
static_assert(sizeof(u16) == 2, "u16 must be two bytes");
static_assert(sizeof(u32) == 4, "u32 must be four bytes");
static_assert(sizeof(u64) == 8, "u64 must be eight bytes");

} // namespace sdhci

#endif // NATIVE_SDHCI_TYPES_H
