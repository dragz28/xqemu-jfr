/*
 * QEMU Xbox System Emulator
 *
 * Copyright (c) 2013 espes
 *
 * Based on pc.c
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_XBOX_H
#define HW_XBOX_H

#define MAX_IDE_BUS 2

void xbox_init_common(QEMUMachineInitArgs *args,
                      uint8_t *default_eeprom,
                      ISABus **out_isa_bus);

#endif
