/*
 * mrv32: mini-rv32ima emulator on MRE platform
 * Date: 15/12/2022
 * Details: This is a combination of mini-rv32ima
 * by Charles Lohr and TelnetVXP's terminal by Ximik Boda.
 * Port to MRE by giangvinhloc610
 */

// MRE headers
#include "vmsys.h"
#include "vmio.h"
#include "vmgraph.h"
#include "vmchset.h"
#include "vmstdlib.h"
#include "ResID.h"
#include "string.h"
#include "vmtimer.h"

// C++ to C call :)
#include "Console_io.h"

// FIFO
#include "fifo.h"

// Macros
#define VRAM_FILE "e:\\rv32ima\\vram.bin"    // Virtual RAM file

#define SCREEN_FPS 20

// mini-rv32ima config macros
#define DTB_SIZE 1536             // DTB size (in bytes), must recount manually each time DTB changes
#define TIME_DIVISOR 1

#ifdef WIN32
#define INSTRS_PER_FLIP 524288    // Number of instructions executed before checking status. See loop()
#else
#define INSTRS_PER_FLIP 2048      // Number of instructions executed before checking status. See loop()
#endif

// Global variables
int scr_w; 
int scr_h;
unsigned int last_wr_addr = 0, last_rd_addr = 0;
unsigned long cycles;

// mini-rv32ima global variables
uint32_t RAM_SIZE = 12 * 1024 * 1024; // Set default RAM amount to 12MB (miniumum RAM tested for booting, you may increase)
int fail_on_all_faults = 0;
uint64_t lastTime = 0;

// vmstate values:
// -1: on startup
//  0: pause
//  1: run
int vmstate = -1; // Pause on start, need to click "continue" button to start (reason: for restoring state if needed)

VMUINT8 *layer_bufs[2] = {0,0};
VMINT layer_hdls[2] = {-1,-1};

int screen_timer_id = -1;

int soc_cycle_timer_id = -1; // emulator cycle timer id

// File handlers for virtual RAM disk
VMFILE vram;

extern fifo_t serial_in;

typedef VMINT(*vm_get_sym_entry_t)(char* symbol);
extern vm_get_sym_entry_t vm_get_sym_entry;

typedef VMINT (*vm_file_seek_t)(VMFILE handle, VMINT offset, VMINT base);
typedef VMINT (*vm_file_read_t)(VMFILE handle, void* data, VMUINT length, VMUINT* nread);
typedef VMINT (*vm_file_write_t)(VMFILE handle, void* data, VMUINT length, VMUINT* written);

vm_file_seek_t vm_file_seek_opt = 0;
vm_file_read_t vm_file_read_opt = 0;
vm_file_write_t vm_file_write_opt = 0;

// If native MRE -> binding functions
#ifndef WIN32
#define malloc vm_malloc
#define free vm_free

void _sbrk(){}
void _write(){}
void _close(){}
void _lseek(){}
void _open(){}
void _read(){}
void _exit(){}
void _getpid(){}
void _kill(){}
void _fstat(){}
void _isatty(){}
#endif

// Event handlers
void handle_sysevt(VMINT message, VMINT param);
void handle_keyevt(VMINT event, VMINT keycode);
void handle_penevt(VMINT event, VMINT x, VMINT y);

// mini-rv32ima helper functions
static uint32_t HandleException(uint32_t ir, uint32_t retval);
static uint32_t HandleControlStore(uint32_t addy, uint32_t val);
static uint32_t HandleControlLoad(uint32_t addy);
static void HandleOtherCSRWrite(uint8_t* image, uint16_t csrno, uint32_t value);

// Load / store helper
static uint32_t store4(uint32_t ofs, uint32_t val);
static uint16_t store2(uint32_t ofs, uint16_t val);
static uint8_t store1(uint32_t ofs, uint8_t val);

static uint32_t load4(uint32_t ofs);
static uint16_t load2(uint32_t ofs);
static uint8_t load1(uint32_t ofs);

// This is the functionality we want to override in the emulator.
//  think of this as the way the emulator's processor is connected to the outside world.
#define MINIRV32WARN( x ) console_str_in( x );
#define MINIRV32_DECORATE  static
#define MINI_RV32_RAM_SIZE RAM_SIZE
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC( pc, ir, retval ) { if( retval > 0 ) { if( fail_on_all_faults ) { console_str_in( "FAULT\n" ); return 3; } else retval = HandleException( ir, retval ); } }
#define MINIRV32_HANDLE_MEM_STORE_CONTROL( addy, val ) if( HandleControlStore( addy, val ) ) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL( addy, rval ) rval = HandleControlLoad( addy );
#define MINIRV32_OTHERCSR_WRITE( csrno, value ) HandleOtherCSRWrite( image, csrno, value );

#define MINIRV32_CUSTOM_MEMORY_BUS
#define MINIRV32_STORE4( ofs, val ) store4(ofs, val)
#define MINIRV32_STORE2( ofs, val ) store2(ofs, val)
#define MINIRV32_STORE1( ofs, val ) store1(ofs, val)
#define MINIRV32_LOAD4( ofs ) load4(ofs)
#define MINIRV32_LOAD2( ofs ) load2(ofs)
#define MINIRV32_LOAD1( ofs ) load1(ofs)

// After all macros have been overwritten, now include the header
#include "mini-rv32ima.h"

struct MiniRV32IMAState core; // core struct

// Main MRE entry point
void vm_main(void){
	vm_file_seek_opt = vm_get_sym_entry("vm_file_seek");
	vm_file_read_opt = vm_get_sym_entry("vm_file_read");
	vm_file_write_opt = vm_get_sym_entry("vm_file_write");

	serial_in = fifo_create(BUF_SIZE, sizeof(int));

	scr_w = vm_graphic_get_screen_width();
	scr_h = vm_graphic_get_screen_height();

	terminal_init();

	vm_reg_sysevt_callback(handle_sysevt);
	vm_reg_keyboard_callback(handle_keyevt);
	vm_reg_pen_callback(handle_penevt);
}

// Render terminal
void draw(){
	t2input_draw(layer_bufs[1]); // Call to C++
	vm_graphic_flush_layer(layer_hdls, 2); // Flush layer
}

// SoC cycle
void socRun(int tid){
	if (vmstate == 1) {
		// Emulator cycle
		uint64_t* this_ccount = ((uint64_t*)&core.cyclel);
		uint32_t elapsedUs = 0;
		elapsedUs = *this_ccount / TIME_DIVISOR - lastTime;
		cycles = *this_ccount; // For calculating the emulated speed
		lastTime += elapsedUs;

		int ret = MiniRV32IMAStep(core, NULL, 0, elapsedUs, INSTRS_PER_FLIP); // Execute upto INSTRS_PER_FLIP cycles before breaking out.
		switch (ret)
		{
			case 0: break;
			case 1: *this_ccount += INSTRS_PER_FLIP; break;
			//case 3: instct = 0; break;
			//case 0x7777: goto restart;  //syscon code for restart
			case 0x5555: console_str_in("POWEROFF!\n"); vmstate = 0; //syscon code for power-off . halt
			default: console_str_in("Unknown failure\n"); break;
		}
	}
}

// Save emulator's state
void save_state() {
	// Do nothing for now
}

// Load state only, not RAM
void load_man() {
	// Do nothing for now
}

// Load emulator's state
void load_state() {
	// Do nothing for now
}

// Refresh screen
void timer(int tid) {
	draw();
}

// Load / store helper
static uint32_t store4(uint32_t ofs, uint32_t val) {
	last_wr_addr = ofs;
	vm_file_seek_opt(vram, ofs, BASE_BEGIN);

	uint32_t r = val;
	VMUINT w;

	vm_file_write_opt(vram, ((uint8_t*)&r), 4, &w);
}

static uint16_t store2(uint32_t ofs, uint16_t val) {
	last_wr_addr = ofs;
	vm_file_seek_opt(vram, ofs, BASE_BEGIN);

	uint16_t r = val;
	VMUINT w;

	vm_file_write_opt(vram, ((uint8_t*)&r), 2, &w);
}

static uint8_t store1(uint32_t ofs, uint8_t val) {
	last_wr_addr = ofs;
	vm_file_seek_opt(vram, ofs, BASE_BEGIN);

	uint8_t r = val;
	VMUINT w;

	vm_file_write_opt(vram, ((uint8_t*)&r), 1, &w);
}

static uint32_t load4(uint32_t ofs) {
	last_rd_addr = ofs;
	vm_file_seek_opt(vram, ofs, BASE_BEGIN);

	uint32_t result;
	VMUINT r;

	vm_file_read_opt(vram, ((uint8_t*)&result), 4, &r);
	return result;
}

static uint16_t load2(uint32_t ofs) {
	last_rd_addr = ofs;
	vm_file_seek_opt(vram, ofs, BASE_BEGIN);

	uint16_t result;
	VMUINT r;

	vm_file_read_opt(vram, ((uint8_t*)&result), 2, &r);
	return result;
}

static uint8_t load1(uint32_t ofs) {
	last_rd_addr = ofs;
	vm_file_seek_opt(vram, ofs, BASE_BEGIN);

	uint8_t result;
	VMUINT r;

	vm_file_read_opt(vram, ((uint8_t*)&result), 1, &r);
	return result;
}

// System event handler
void handle_sysevt(VMINT message, VMINT param) {
	VMWCHAR sd_path[100];
	VMWCHAR vram_path[100];
	unsigned char zero_array[1024] = {0};
	switch (message) {
	case VM_MSG_CREATE:
	case VM_MSG_ACTIVE:
		layer_hdls[0] = vm_graphic_create_layer(0, 0, scr_w, scr_h, -1);
		layer_hdls[1] = vm_graphic_create_layer(0, 0, scr_w, scr_h, tr_color);
		
		vm_graphic_set_clip(0, 0, scr_w, scr_h);

		layer_bufs[0]=vm_graphic_get_layer_buffer(layer_hdls[0]);
		layer_bufs[1]=vm_graphic_get_layer_buffer(layer_hdls[1]);

		vm_switch_power_saving_mode(turn_off_mode);

		set_layer_handler(layer_bufs[0], layer_bufs[1], layer_hdls[1]); // Call to C++

		// init code

		if (message == VM_MSG_CREATE) {
			// Convert file path to ucs2
			vm_gb2312_to_ucs2(vram_path, 1000, VRAM_FILE);

			// Open RAM file
			vram = vm_file_open(vram_path, // Virtual ram file (you can change yourself)
				MODE_APPEND,               // Open in append mode
				VM_TRUE);                  // Open in binary mode

			// Allocate space for core struct
			memset(&core, 0, sizeof(struct MiniRV32IMAState));

			// Setup core
			core.pc = MINIRV32_RAM_IMAGE_OFFSET;
			core.regs[10] = 0x00; //hart ID
			core.regs[11] = RAM_SIZE - sizeof(struct MiniRV32IMAState) - DTB_SIZE + MINIRV32_RAM_IMAGE_OFFSET; // dtb_pa (Must be valid pointer) (Should be pointer to dtb)
			core.extraflags |= 3; // Machine-mode.
		}

		if(soc_cycle_timer_id == -1)
			soc_cycle_timer_id = vm_create_timer(0, socRun);

		if(screen_timer_id==-1)
			screen_timer_id = vm_create_timer(1000/SCREEN_FPS, timer); // terminal refresh
		break;
		
	case VM_MSG_PAINT:
		draw();
		break;
		
	case VM_MSG_INACTIVE:
		vm_switch_power_saving_mode(turn_on_mode);
		if( layer_hdls[0] != -1 ){
			vm_graphic_delete_layer(layer_hdls[1]);
			vm_graphic_delete_layer(layer_hdls[0]);
			layer_hdls[0] = -1;
		}

		// Delete timers
		if(soc_cycle_timer_id != -1)
			vm_delete_timer(soc_cycle_timer_id);
		soc_cycle_timer_id = -1;
		if(screen_timer_id!=-1)
			vm_delete_timer(screen_timer_id);
		screen_timer_id = -1;

		// Commit data to file
		//vm_file_commit(sd);

		// Close file handlers
		//vm_file_close(sd);
		//vm_file_close(vram);
		break;	
	case VM_MSG_QUIT:
		if( layer_hdls[0] != -1 ){
			vm_graphic_delete_layer(layer_hdls[0]);
			vm_graphic_delete_layer(layer_hdls[1]);
		}

		if(soc_cycle_timer_id != -1)
			vm_delete_timer(soc_cycle_timer_id);
		if(screen_timer_id!=-1)
			vm_delete_timer(screen_timer_id);

		// Close file handlers
		vm_file_close(vram);
		break;	
	}
}

// Keyboard event handler
void handle_keyevt(VMINT event, VMINT keycode) {
#ifdef WIN32
	if(keycode>=VM_KEY_NUM1&&keycode<=VM_KEY_NUM3)
		keycode+=6;
	else if(keycode>=VM_KEY_NUM7&&keycode<=VM_KEY_NUM9)
		keycode-=6;
#endif
	t2input_handle_keyevt(event, keycode);
}

// Touch event handler
void handle_penevt(VMINT event, VMINT x, VMINT y){
	t2input_handle_penevt(event, x, y);
	draw();
}

// mini-rv32ima exception handlers

static uint32_t HandleException(uint32_t ir, uint32_t code)
{
	// Weird opcode emitted by duktape on exit.
	if (code == 3)
	{
		// Could handle other opcodes here.
	}
	return code;
}

static uint32_t HandleControlStore(uint32_t addy, uint32_t val)
{
	if (addy == 0x10000000) // UART 8250 / 16550 Data Buffer
	{
		char out[8];
		sprintf(out, "%c", val);
		console_str_in(out);
	}
	return 0;
}


static uint32_t HandleControlLoad(uint32_t addy)
{
	// Emulating a 8250 / 16550 UART

	if (addy == 0x10000005) {
		return 0x60 | (!fifo_is_empty(serial_in));
	}

	else if (addy == 0x10000000 && (!fifo_is_empty(serial_in))) {
		int ret;
		fifo_get(serial_in, &ret);
		return ret;
	}

	return 0;
}

static void HandleOtherCSRWrite(uint8_t* image, uint16_t csrno, uint32_t value)
{
	if (csrno == 0x136)
	{
		char out[32];
		sprintf(out, "%d", value);
		console_str_in(out);
	}
	if (csrno == 0x137)
	{
		char out[32];
		sprintf(out, "%08x", value);
		console_str_in(out);
	}
}
