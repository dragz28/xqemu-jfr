/*
 * QEMU MCPX internal ROM implementation
 *
 * Copyright (c) 2014 espes
 * Copyright (c) 2014 Jannik Vogel
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

/*
    Notes:

    - For the COMPLEX bios we need proper emulation though..
      However, there are only 2 known exploits, so it should be enough to
      set EIP in OUTB and POKEPCI
    - Passes execution to flash rom or bootloader.c
*/

//FIXME: Formating and probably other code QEMU conventions violated!
//FIXME: Add more checks for mcpx_rom_enabled in emulation
//FIXME: replace phsical memory access etc by cpu_ldl_kernel etc
//FIXME: Add option to enable/disable debug printf

//FIXME: Replace "2bl" by "bootloader"

//FIXME: tons of useless include here
#include "hw/pci/pci.h" /* FIXME: included for the address_space_memory which should be included from elsewhere */
#include "hw/hw.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "exec/memory.h"
#include "qemu/config-file.h"
#include "target-i386/helper.h"
#include "hw/xbox/xbox.h"
#include "exec/cpu-all.h"

#include "hw/xbox/mcpx.h"

#include "hw/xbox/bootloader.h"
#include "hw/xbox/rc4.h" // UGLY.. do this differently?

#include "keys.fox" //FIXME: Remove once everything has a cli
#define warningPrintf(x,...) printf(x, ## __VA_ARGS__)
#define errorPrintf(x,...) fprintf(stderr,x, ## __VA_ARGS__);
#define debugPrintf(x,...) fprintf(stdout,x, ## __VA_ARGS__);

static const hwaddr bootrom_addr = 0xFFFFFE00;
static const hwaddr bootrom_dt_offset = +0x1D8;
static const hwaddr bootrom_dt_pointer_offset = +0x1F4;
static unsigned int bootrom_size = 0x200; //FIXME: Better type


static void mcpx_rom_switch_to_protected_mode(struct XBOX_MCPXState* s)
{

  X86CPU *cpu = X86_CPU(first_cpu);
  CPUX86State *env = &cpu->env;

  // Read descriptor information from pointer
  uint16_t limit;
  uint32_t base;
  cpu_physical_memory_read(bootrom_addr+bootrom_dt_pointer_offset+0, &limit, 2);
  cpu_physical_memory_read(bootrom_addr+bootrom_dt_pointer_offset+2, &base, 4);

  // Set GDT and IDT
  env->gdt.limit = limit;
  env->gdt.base = base;
  env->idt.limit = limit;
  env->idt.base = base;

  // Enable protected mode
  cpu_x86_update_cr0(env, env->cr[0] | 1);
  helper_ljmp_protected(env, 0x8, 0xfffffe00, 0); //FIXME: What's up with the last argument?!

  // Set up segments
  helper_load_seg(env, R_DS, 0x10);
  helper_load_seg(env, R_ES, 0x10);
  helper_load_seg(env, R_SS, 0x10);

}

static void mcpx_rom_run_xcode(struct XBOX_MCPXState* s)
{
  
  X86CPU *cpu = X86_CPU(first_cpu);
  CPUX86State *env = &cpu->env;

  // Only supports X3 because X2 is not actually part of the MCPX

  uint8_t opcode;
  uint32_t op1;
  uint32_t op2;

  uint32_t pc = 0xFF000080;
  uint32_t acc = 0x00000000;

  char op1_string[2+8+1];
  char op2_string[2+8+1];

  debugPrintf("Starting XCode interpreter\n");

  bool done = false;
  while(!done) {

    if (!s->rom.enabled) {
      break;
    }

    //FIXME: How to use segmentation here?!
  //  debugPrintf("Reading opcode from 0x%08X\n",pc);
    cpu_physical_memory_read(pc+0,&opcode,1);
//    debugPrintf("Read opcode 0x%02X\n",opcode);
    cpu_physical_memory_read(pc+1,&op1,4);
    cpu_physical_memory_read(pc+5,&op2,4);

    if (opcode == 0x07) { // (prefix)
      opcode = op2;
      op1 = op2;
      op2 = acc;
      strcpy(op2_string,op1_string);
      strcpy(op1_string,"ACC");
    }

    sprintf(op1_string,"0x%08X",op1);
    sprintf(op2_string,"0x%08X",op2);

    printf("XCode 0x%08X: ",pc);


    switch (opcode) {
      case 0x02: // PEEK
        if (s->revision == 0xB1) {
          printf("IF (%s <= 0xFF000000) THEN ACC := MEM[%s]\n",op1_string,op1_string);
          if (op1 <= 0xFF000000) {
            cpu_physical_memory_read(op1,&acc,4);
          }
        } else {
          //FIXME: Verify!
          printf("ACC := MEM[%s & 0x0FFFFFFF]\n",op1_string);
          cpu_physical_memory_read(op1 & 0x0FFFFFFF,&acc,4);
        }
        break;
      case 0x03: // POKE
        printf("MEM[%s] := %s\n",op1_string,op2_string);
        cpu_physical_memory_write(op1,&op2,4);
        break;
      case 0x04: // POKEPCI
        printf("PCICONF[%s] := %s\n",op1_string,op2_string);
        //FIXME: Add broken security check to block "disable the MCPX ROM"
        helper_outl(0xCF8,op1);  
        helper_outl(0xCFC,op2);  
        env->eip = 0xfffffe62;
        break;
      case 0x05: // PEEKPCI      
        printf("ACC := PCICONF[%s]\n",op1_string);
        helper_outl(0xCF8,op1);  
        acc = helper_inl(0xCFC);  
        break;
      case 0x06: // AND/OR       
        printf("ACC := (ACC & %s) | %s\n",op1_string,op2_string);
        acc = (acc & op1) | op2;
        break;
      case 0x08: // BNE
        printf("IF ACC <> %s THEN PC := PC + %s\n",op1_string,op2_string);
        if (acc != op1) { pc += op2; }
        break;
      case 0x09: // BRA          
        printf("PC := PC + %s\n",op2_string);
        pc += op2;
        break;
      case 0x11: // OUTB
        printf("PORT[%s & 0xFFFF] := %s\n",op1_string,op2_string);
        helper_outb(op1,op2);
        env->eip = 0xfffffea0;
        break;
      case 0x12: // INB
        printf("ACC := PORT[%s & 0xFFFF]\n",op1_string);
        acc = helper_inb(op1);
        break;
      case 0xEE: // END
        printf("MTRR_SETUP(%s)\n",op1_string);
        done = true;
        break;
      case 0x01: // UNUSED 
      case 0x80: // UNUSED (Used to trap X2?)
      case 0xF5: // UNUSED (Used to trap X2?)
        printf("NOP() \n");
        break;
      default:
        printf("Unknown XCode instruction 0x%02X,%s,%s\n",opcode,op1_string,op2_string);
    }

    pc += 9;

  }

  /* Update eip even though it shouldn't matter much anymore */
  env->eip = 0xfffffebc;

  env->regs[R_ESI] = pc;
  env->regs[R_EBX] = op1;
  env->regs[R_ECX] = op2;
  env->regs[R_EDI] = acc;

}

// FIXME: Verify against actual bootrom
static void mcpx_rom_setup_mtrr(struct XBOX_MCPXState* s)
{

  X86CPU *cpu = X86_CPU(first_cpu);
  CPUX86State *env = &cpu->env;

  // Setup MTRR
  unsigned int i;
  env->regs[R_EAX] = 0x00000000;
  env->regs[R_EDX] = 0x00000000;
  for(i = 0; i < 8; i++) {
    env->regs[R_ECX] = MSR_MTRRphysBase(i);
    helper_wrmsr(env);
  }
  for(i = 0; i < 8; i++) {
    env->regs[R_ECX] = MSR_MTRRphysMask(i);
    helper_wrmsr(env);
  }
  env->regs[R_ECX] = MSR_MTRRdefType;
  env->regs[R_EAX] = env->regs[R_EBX];
  helper_wrmsr(env);

  // Enable caching
  cpu_x86_update_cr0(env, env->cr[0] & 0x9fffffff);

}

static void mcpx_rom_emulate(XBOX_MCPXState* s)
{

  printf("MCPX HLE!\n");

  X86CPU *cpu = X86_CPU(first_cpu);
  CPUX86State *env = &cpu->env;

  // Switch to 32 bit mode and use GDT from MCPX rom
  mcpx_rom_switch_to_protected_mode(s);

  // Run XCode [0xFF000080]
  mcpx_rom_run_xcode(s);

  // Oops! We are probably running a hacked ROM which turned the MCPX off
  if (!s->rom.enabled) {
    return;
  }

  // Setup MTRR with register from xcode
  mcpx_rom_setup_mtrr(s); 

  // Now see if we HLE the 2BL too
  const uint8_t* key_2bl = NULL; //jayfoxrox_key_2bl; //FIXME read key from command line
  
  if (key_2bl) {
    s->rom.hle_2bl_code = false;

    // Decrypt 2bl from flash    
    struct rc4_state rc4;
    rc4_init(&rc4,key_2bl,16);
    size_t length = 0x6000;
    uint8_t* buffer = g_malloc(length);
    cpu_physical_memory_read(0xFFFF9E00, buffer, length);
    rc4_crypt(&rc4,buffer,buffer,length);
    cpu_physical_memory_write(0x00090000, buffer, length);
    g_free(buffer);

    /* Check if it's valid */
    if (s->xmode == 3) {
      if (s->revision >= 0xC3) {
        // TEA
        //FIXME: Can only be done if we know the actual hash - is that legal?
      } else {
        // 32 bit check
        uint32_t magic;
        cpu_physical_memory_read(0x00095fe4, &magic, 4);
        assert(magic == jayfoxrox_2bl_hash); //FIXME: Is sharing this value legal?
        debugPrintf("Check passed. Magic: 0x%08X?!\n",magic);
      }
    }

    // Now jump to 2bl
    uint32_t entryPoint; // FIXME: Better type?
    cpu_physical_memory_read(0x00090000, &entryPoint, 4);
    debugPrintf("Booting 2bl from 0x%08X\n",entryPoint);
    env->regs[R_ESI] = 0x8f000; // S-Box location - is this necessary?!
    env->regs[R_EDI] = length; // 2bl size
    env->eip = env->regs[R_EAX] = entryPoint;
  } else {
    /* Go to 2bl HLE */
    s->rom.hle_2bl_code = true;
    /* We have to tell the bootloader what it was compiled for */
    bootloader_emulate(true, s->xmode == 3, s->revision >= 0xC3, false);
  }
}

static int load_mcpx_rom(XBOX_MCPXState* s, const char* bootrom_file) {
  char *filename;
  int rc, fd = -1;
  if (bootrom_file
      && (filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bootrom_file))) {
    size_t size = get_image_size(filename);

    if (size != bootrom_size) {
      fprintf(stderr, "MCPX bootrom should be %d bytes, got %d\n",
              bootrom_size,size);
      return -1;
    }

    fd = open(filename, O_RDONLY | O_BINARY);
    assert(fd != -1);
    rc = read(fd, s->rom.bootrom_data, size);
    assert(rc == size);

    printf("MCPX rom loaded\n");

    close(fd);
  }
  return 0;
}


void mcpx_rom_hide(struct XBOX_MCPXState* s)
{
  debugPrintf("MCPX is hidden now\n");

  if (s->rom.enabled) {
    // Restore flash image
    cpu_physical_memory_write_rom(&address_space_memory, bootrom_addr,
                                  s->rom.flash_data,
                                  bootrom_size);
    printf("flash image written!\n");
  }
  s->rom.enabled = false;
}

void mcpx_rom_show(struct XBOX_MCPXState* s) {
  debugPrintf("MCPX is shown now\n");

  if (!s->rom.enabled) {

    /* qemu's memory region shit is actually kinda broken -
     * Trying to execute off a non-page-aligned memory region
     * is fucked, so we can't just map in the bootrom.
     *
     * We need to be able to disable it at runtime, and
     * it shouldn't be visible ontop of the bios mirrors. It'll have to
     * be a retarded hack.
     *
     * Be lazy for now and just write it ontop of the bios.
     *
     * (We do this here since loader.c loads roms into memory in a reset
     * handler, and here we /should/ be handled after it.)
     */

    //Create backup of flash below
    cpu_physical_memory_read(bootrom_addr,
                             s->rom.flash_data,
                             bootrom_size);

    // And overlay the area with the MCPX ROM (generated or from ROM)
    if (s->rom.hle_mcpx_rom_code) {

      // GDT/IDT
      const uint32_t dt[] = { // 24 bytes
        0x00000000,0x00000000, //+0  = 0,1
        0x0000ffff,0x00CF9b00, //+8  = 2,3
        0x0000ffff,0x00CF9300  //+16 = 4,5
      };
      cpu_physical_memory_write_rom(&address_space_memory, bootrom_addr+bootrom_dt_offset,
                                    dt,
                                    sizeof(dt));

      // FWORD Pointer to GDT/IDT descriptor
      const uint8_t dt_pointer[] = {
        0x18,0x00, // This seems like a Microsoft bug? Doesn't the size have to be 0x17 (0x18-1)?
        0xd8,0xff,0xff,0xff
      };
      cpu_physical_memory_write_rom(&address_space_memory, bootrom_addr+bootrom_dt_pointer_offset,
                                    dt_pointer,
                                    sizeof(dt_pointer));

    } else {
      cpu_physical_memory_write_rom(&address_space_memory, bootrom_addr,
                                    s->rom.bootrom_data,
                                    bootrom_size);
      printf("image written to memory\n");
    }
  }

  s->rom.enabled = true;

}

static void mcpx_rom_reset_late(struct XBOX_MCPXState* s)
{

  printf("Late reset! 0x%X\n",s);

  // Make the MCPX image visible
  mcpx_rom_show(s);

  if (s->rom.hle_mcpx_rom_code) {
    // Emulate the MCPX startup
    //FIXME: Make sure we are at 0xFFFF:FFF0
    mcpx_rom_emulate(s);
  } else {
    printf("Real bios should take over now!\n");
  }

  qemu_unregister_reset(mcpx_rom_reset_late, s);
 
}

void mcpx_rom_init(struct XBOX_MCPXState* s)
{

  // Make sure we keep track wether the MCPX is enabled or not
  s->rom.enabled = false;

debugPrintf("in rom init, booting X%X, revision 0x%02X\n",s->xmode,s->revision);

  const char* mcpx_rom = NULL;
  QemuOpts *machine_opts = qemu_opts_find(qemu_find_opts("machine"), 0);
  if (machine_opts) {
    mcpx_rom = qemu_opt_get(machine_opts, "mcpx_rom");
/*
FIXME: Would this work?
    s->revision = qemu_opt_get(machine_opts, "mcpx_revision");
    s->xmode = qemu_opt_get(machine_opts, "mcpx_xmode");
*/
  }

  // So we have to create a virtual rom..
  if (mcpx_rom) {
    s->rom.hle_mcpx_rom_code = false;
    // Load the MCPX ROM from file  
    load_mcpx_rom(s,mcpx_rom);
  } else {
    s->rom.hle_mcpx_rom_code = true;
  }

  return;  
}


void mcpx_rom_reset(struct XBOX_MCPXState* s)
{
  qemu_register_reset(mcpx_rom_reset_late, s);
}
