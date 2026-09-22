// license:BSD-3-Clause
// copyright-holders:Aaron Giles, Nathan Woods
/***************************************************************************

    v9938 / v9958 emulation -- SRZ80 card core

    This file is a port of the MAME 0.289 device
    src/devices/video/v9938.cpp (kept unmodified beside it as v9938.cpp)
    into a self-contained, host-free core that an SRZ80 card plugin drives.

    Only the MAME framework seams changed; the VDP logic is the original
    code.  Each changed site carries a "PORT:" comment.  The seams are:

      device_memory_interface / m_vram_space->read_byte|write_byte
                                     -> m_vram[] byte array (256 KiB + 64 KiB)
      device_palette_interface        -> m_pen16[] / m_pen256[] tables
      device_video_interface /
        screen(), bitmap_rgb32         -> refresh_framebuffer() into an
                                          internal RGBA8 surface
      emu_timer / attotime             -> the card calls line_tick() once per
                                          scanline
      devcb_write_line m_int_callback  -> irq_line() virtual, driven by the card
      save_item / NAME                 -> vram_size() snapshot helpers
      machine().rand()                 -> rng() virtual
      LOGMASKED                        -> no-op (see the LOGMASKED definition)
      BIT(x)                           -> local constexpr
      rgb_t                            -> local rgb32() constexpr

    Rendering notes for reviewers:

      MAME renders every mode into a uint32_t line of internal RGB
      (0x00RRGGBB) and lets the screen device convert that to the output
      bitmap.  This port keeps the same mode functions and the same internal
      RGB values, converts each finished line once (bg_convert_line) into the
      engine's SRH_VIDEO_RGBA8 byte order, and applies R/B expansion at the
      same time, so no per-pixel work is duplicated.

      MAME's m_bitmap is also its save-state backing store.  Here it is a
      pure rendering scratch buffer and is not part of save state.

***************************************************************************/

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace srz80::vdp {

// -------------------------------------------------------------------------
// PORT: replaced the MAME headers (emu.h, v9938.h).  Everything below this
// block that looks like MAME scaffolding is a local stand-in for it.
// -------------------------------------------------------------------------

// PORT: the model selectors were macros in MAME; they are typed constants
// here so a card can name them without the preprocessor leaking a
// replacement into unrelated identifiers.
constexpr int MODEL_V9938 = 0;
constexpr int MODEL_V9958 = 1;

#define EXPMEM_OFFSET 0x20000
#define V9938_LONG_WIDTH (512 + 32)
#define VRAM_TOTAL_SIZE 0x30000

// PORT: emu.h's fixed-width aliases.
using u8 = uint8_t;

// PORT: emucore.h's BIT().  MAME's macro takes (value, bit), which is the form
// the original v9938.cpp uses, so the port keeps the same shape.
constexpr uint32_t BIT2(uint32_t value, int bit) { return (value >> bit) & 1u; }

// PORT: emu.h/logmacro.h.  Logging is intentionally silent in a card; the
// sites are kept so the port stays diffable against the original.
#define LOG_GENERAL 0u
#define LOG_WARN (1U << 1)
#define LOG_INT (1U << 2)
#define LOG_STATUS (1U << 3)
#define LOG_REGWRITE (1U << 4)
#define LOG_COMMAND (1U << 5)
#define LOG_MODE (1U << 6)
#define LOG_NOTIMP (1U << 7)
#define LOG_DETAIL (1U << 8)
#define VERBOSE (LOG_GENERAL | LOG_WARN)
#define LOGMASKED(mask, ...) ((void)0)

// PORT: emucore.h's pen_t.  Internal colors stay in MAME's R,G,B byte order.
using pen_t = uint32_t;

// PORT: lib/util/palette.h's pal3bit/pal5bit and rgb_t.
//
// MAME's bitmap holds full 8-bit channels, so its palette helpers expand a
// 3-bit V9938 component to 8 bits and rgb_t packs those bytes.  The port must
// keep that order of operations: convert_line() later compresses the 8-bit
// channel back down for the engine surface.  Packing raw 3-bit values here
// would scale them twice, turning COMP=4 into 0x21 instead of 0x24.
constexpr uint8_t palexpand3(uint8_t bits) { return uint8_t((bits << 5) | (bits << 2) | (bits >> 1)); }
constexpr uint8_t palexpand5(uint8_t bits) { return uint8_t((bits << 3) | (bits >> 2)); }
constexpr uint8_t pal3bit(uint8_t bits) { return palexpand3(uint8_t(bits & 7u)); }
constexpr uint8_t pal5bit(uint8_t bits) { return palexpand5(uint8_t(bits & 31u)); }
constexpr uint32_t rgb32(uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t{0xFF} << 24) | (uint32_t{r} << 16) | (uint32_t{g} << 8) | uint32_t{b};
}
// rgb_t(pal3bit(r), pal3bit(g), pal3bit(b)): the exact form every original
// call site uses, so the arguments are raw 3-bit components (0..7).
constexpr uint32_t rgb3(uint8_t r, uint8_t g, uint8_t b) {
    return rgb32(pal3bit(r), pal3bit(g), pal3bit(b));
}
constexpr uint32_t rgb5(uint8_t r, uint8_t g, uint8_t b) {
    return rgb32(pal5bit(r), pal5bit(g), pal5bit(b));
}

enum {
    V9938_MODE_TEXT1 = 0,
    V9938_MODE_MULTI,
    V9938_MODE_GRAPHIC1,
    V9938_MODE_GRAPHIC2,
    V9938_MODE_GRAPHIC3,
    V9938_MODE_GRAPHIC4,
    V9938_MODE_GRAPHIC5,
    V9938_MODE_GRAPHIC6,
    V9938_MODE_GRAPHIC7,
    V9938_MODE_TEXT2,
    V9938_MODE_UNKNOWN
};

static const char *const v9938_modes[] = {
    "TEXT 1", "MULTICOLOR", "GRAPHIC 1", "GRAPHIC 2", "GRAPHIC 3",
    "GRAPHIC 4", "GRAPHIC 5", "GRAPHIC 6", "GRAPHIC 7", "TEXT 2",
    "UNKNOWN"
};

// ======================> v99x8_device

class v99x8_device
{
public:
    static constexpr int HTOTAL = 684;
    static constexpr int HVISIBLE = 544;
    static constexpr int VTOTAL_NTSC = 262;
    static constexpr int VTOTAL_PAL = 313;
    static constexpr int VVISIBLE_NTSC = 26 + 192 + 25;
    static constexpr int VVISIBLE_PAL = 53 + 192 + 49;
    // Looking at some youtube videos of real units on real monitors
    // there appear to be small vertical timing differences. Some (LCD)
    // monitors show the full borders, other CRT monitors seem to
    // display ~5 lines less at the top and bottom of the screen.
    static constexpr int VERTICAL_ADJUST = 5;
    static constexpr int TOP_ERASE = 13;
    static constexpr int VERTICAL_SYNC = 3;

    // PORT: fixed part of the state blob: m_pal_reg, m_stat_reg, m_cont_reg,
    // the scalar group, the mouse pair, five byte fields and the address latch.
    static constexpr uint64_t STATE_HEADER_SIZE =
        sizeof(uint8_t) * 32 + sizeof(uint8_t) * 10 + sizeof(uint8_t) * 48 +
        sizeof(int32_t) * 17 + sizeof(int16_t) * 2 + 5 + sizeof(uint16_t);

    static constexpr uint64_t COMMAND_STATE_SIZE = 13 * sizeof(int32_t) + 5;

    // PORT: the engine surface.  Width is HVISIBLE; height is the current
    // model's doubled line count, and the buffer is allocated for the taller
    // PAL frame so a mode switch never reallocates.
    static constexpr uint32_t surface_width = HVISIBLE;
    static constexpr uint32_t surface_max_height = VTOTAL_PAL * 2;

    v99x8_device(int model, uint32_t vram_size);
    virtual ~v99x8_device() = default;

    // ---- CPU port interface (the card maps these at its four IO ports) ----
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t data);
    // The four register-level entry points MAME's read()/write() dispatched to.
    uint8_t vram_r();
    uint8_t status_r();
    void palette_w(uint8_t data);
    void vram_w(uint8_t data);
    void command_w(uint8_t data);
    void register_w(uint8_t data);

    // ---- raster ----
    // One scanline.  The card calls this once per line period.
    void line_tick();

    // ---- video ----
    const uint8_t *framebuffer() const { return m_frame.data(); }
    uint32_t framebuffer_size() const { return surface_width * surface_max_height * 4u; }
    uint32_t scanout_line() const { return uint32_t(m_scanline); }
    uint32_t scanout_lines() const { return uint32_t(m_height); }
    uint32_t visible_height() const { return uint32_t(m_height) * 2u; }
    int palette_mode() const { return m_pal_ntsc; }
    int mode() const { return m_mode; }
    const char *mode_name() const { return v9938_modes[m_mode]; }
    const uint8_t *status_registers() const { return m_stat_reg; }
    const uint8_t *control_registers() const { return m_cont_reg; }
    uint32_t vram_size() const { return uint32_t(m_vram_size); }
    // Raw VRAM byte for inspection (device inspector, tests, phase 3 tools).
    uint8_t vram_byte(int offset) const { return vram_barrier(offset); }

    // ---- color bus (mouse / lightpen, registers 8 and 11) ----
    void colorbus_x_input(int mx_delta);
    void colorbus_y_input(int my_delta);
    void colorbus_button_input(bool button1, bool button2);

    // Clears the rendered surface.  A cold reset restarts the chip's picture
    // from black without waiting for the raster to overwrite every row.
    void clear_frame() {
        std::fill(m_frame.begin(), m_frame.end(), uint8_t{0});
        for (size_t offset = 3; offset < m_frame.size(); offset += 4)
            m_frame[offset] = 0xff;
        std::fill(m_bitmap.begin(), m_bitmap.end(), pen_t{0});
    }

    // ---- state ----
    // Field-wise snapshot of everything save_item() covered, plus VRAM.
    uint64_t state_size() const;
    void save_state(uint8_t *buffer) const;
    bool load_state(const uint8_t *buffer, uint64_t size);

protected:
    // construction/destruction
    void device_start();
    void device_reset();
    void clear_vram() { std::fill(m_vram.begin(), m_vram.end(), uint8_t{0}); }
    void select_model(int model) { m_model = model; }

    // PORT: MAME had a v9938_device and a v9958_device override; the port is
    // one class selected by m_model, so this has a base implementation that
    // only the V9958 pays for.  A card may still override it.
    virtual void palette_init();
    virtual void irq_line(uint8_t state) = 0;

    void update_line();

private:
    // internal helpers
    pen_t pen16(int index) const { return m_pen16[index]; }
    pen_t pen256(int index) const { return m_pen256[index]; }
    // PORT: device_palette_interface's set_pen_color + rgb_t::set_a.  The
    // alpha bit is kept even though the engine surface is opaque, because it
    // is what makes pen 0 transparent on real hardware.
    void set_pen16(int index, pen_t pen) { m_pen16[index] = pen | (index != 0 ? 0xff000000u : 0u); }
    void set_pen256(int index, pen_t pen) { m_pen256[index] = pen | (index != 0 ? 0xff000000u : 0u); }

    static inline int position_offset(uint8_t value) { value &= 0x0f; return (value < 8) ? -value : 16 - value; }
    void reset_palette();
    void vram_write(int offset, int data);
    int vram_read(int offset);
    void check_int();
    void register_write(int reg, int data);

    void default_border(uint32_t *ln);
    void graphic7_border(uint32_t *ln);
    void graphic5_border(uint32_t *ln);
    void mode_text1(uint32_t *ln, int line);
    void mode_text2(uint32_t *ln, int line);
    void mode_multi(uint32_t *ln, int line);
    void mode_graphic1(uint32_t *ln, int line);
    void mode_graphic23(uint32_t *ln, int line);
    void mode_graphic4(uint32_t *ln, int line);
    void mode_graphic5(uint32_t *ln, int line);
    void mode_graphic6(uint32_t *ln, int line);
    void mode_graphic7(uint32_t *ln, int line);
    void mode_unknown(uint32_t *ln, int line);
    void default_draw_sprite(uint32_t *ln, uint8_t *col);
    void graphic5_draw_sprite(uint32_t *ln, uint8_t *col);
    void graphic7_draw_sprite(uint32_t *ln, uint8_t *col);

    void sprite_mode1(int line, uint8_t *col);
    void sprite_mode2(int line, uint8_t *col);
    void set_mode();
    void refresh_32(int line);
    void refresh_line(int line);

    void interrupt_start_vblank();
	int VDPVRMP(uint8_t M, int MX, int X, int Y);

	uint8_t VDPpoint5(int MXS, int SX, int SY);
	uint8_t VDPpoint6(int MXS, int SX, int SY);
	uint8_t VDPpoint7(int MXS, int SX, int SY);
	uint8_t VDPpoint8(int MXS, int SX, int SY);

	uint8_t VDPpoint(uint8_t SM, int MXS, int SX, int SY);

	void VDPpsetlowlevel(int addr, uint8_t CL, uint8_t M, uint8_t OP);

	void VDPpset5(int MXD, int DX, int DY, uint8_t CL, uint8_t OP);
	void VDPpset6(int MXD, int DX, int DY, uint8_t CL, uint8_t OP);
	void VDPpset7(int MXD, int DX, int DY, uint8_t CL, uint8_t OP);
	void VDPpset8(int MXD, int DX, int DY, uint8_t CL, uint8_t OP);

	void VDPpset(uint8_t SM, int MXD, int DX, int DY, uint8_t CL, uint8_t OP);

	int get_vdp_timing_value(const int *);

	void srch_engine();
	void line_engine();
	void lmmv_engine();
	void lmmm_engine();
	void lmcm_engine();
	void lmmc_engine();
	void hmmv_engine();
	void hmmm_engine();
	void ymmm_engine();
	void hmmc_engine();


    void device_post_load();
    void update_command();
    void cpu_to_vdp(uint8_t V);
    uint8_t vdp_to_cpu();
    void report_vdp_command(uint8_t Op);
    uint8_t command_unit_w(uint8_t Op);

    inline bool v9938_second_field();

    // PORT: MAME's screen().configure() plus the display-height bookkeeping
    // that used to live in set_screen_parameters()/configure_pal_ntsc().
    void update_display_parameters();
    // PORT: MAME's bitmap_rgb32/pen_t line conversion straight into the
    // engine's RGBA8 byte order.
    void convert_line(int line);
    // PORT: m_bitmap.pix(y) became a plain row pointer, clamped because the
    // scratch buffer holds the taller PAL frame.
    uint32_t *bitmap_line(int y);
    // PORT: the renderers indexed m_vram_space directly and relied on its
    // global_mask plus the device_start() 0xff pad.  vram_read()/vram_write()
    // keep the GRAPHIC 6/7 interleave; these two are the raw accessors the
    // renderers use.
    uint8_t vram_barrier(int offset) const;
    void vram_barrier_w(int offset, uint8_t data);
    // PORT: the palette tables are derived from m_pal_reg, so a state load
    // rebuilds them rather than restoring them.
    void rebuild_palette_registers();

    // PORT: machine().rand() replacement.  The card seeds it deterministically.
    virtual uint32_t rng() { return 0; }

	// Command unit
	struct {
		int SX,SY;
		int DX,DY;
		int TX,TY;
		int NX,NY;
		int MX;
		int ASX,ADX,ANX;
		uint8_t CL;
		uint8_t LO;
		uint8_t CM;
		uint8_t MXS, MXD;
	} m_mmc{};
	int  m_vdp_ops_count = 0;
	void (v99x8_device::*m_vdp_engine)() = nullptr;

    // general
    int m_offset_x, m_offset_y, m_visible_y, m_mode;
    // palette
    int m_pal_write_first, m_cmd_write_first;
    uint8_t m_pal_write, m_cmd_write;
    uint8_t m_pal_reg[32], m_stat_reg[10], m_cont_reg[48], m_read_ahead;
    uint8_t m_v9958_sp_mode;
    uint8_t m_second_field;

    // memory
    uint16_t m_address_latch;
    int m_vram_size;
    // PORT: device_memory_interface's VRAM address space.
    std::vector<uint8_t> m_vram;

    // interrupt
    uint8_t m_int_state;
    int m_scanline;
    // blinking
    int m_blink, m_blink_count;
    // mouse
    int16_t m_mx_delta, m_my_delta;
    // mouse & lightpen
    uint8_t m_button_state;

    // PORT: device_palette_interface's palette entries.
    pen_t m_pen16[16];
    pen_t m_pen256[256];

    // PORT: render buffers.  MAME allocated m_bitmap through the screen
    // device and let the mode functions write it in place; here it is a plain
    // [surface_max_height][surface_width] buffer plus one spare line that the
    // GRAPHIC 5/6/7 sprite blitters can use.
    std::vector<uint32_t> m_bitmap;
    std::vector<uint32_t> m_scratch_line;
    std::vector<uint8_t> m_frame;

    // video timing
    uint8_t m_pal_ntsc;
    int m_scanline_start;
    int m_vblank_start;
    int m_scanline_max;
    int m_height;
    int m_model;

    struct v99x8_mode
    {
        uint8_t m;
        void (v99x8_device::*visible_32)(uint32_t*, int);
        void (v99x8_device::*border_32)(uint32_t*);
        void (v99x8_device::*sprites)(int, uint8_t*);
        void (v99x8_device::*draw_sprite_32)(uint32_t*, uint8_t*);
    };
    static const v99x8_mode s_modes[];
};

// =====================================================================
//  Mode table
// =====================================================================

const v99x8_device::v99x8_mode v99x8_device::s_modes[] = {
    { 0x02,
        &v99x8_device::mode_text1,
        &v99x8_device::default_border,
        nullptr,
        nullptr
    },
    { 0x01,
        &v99x8_device::mode_multi,
        &v99x8_device::default_border,
        &v99x8_device::sprite_mode1,
        &v99x8_device::default_draw_sprite
    },
    { 0x00,
        &v99x8_device::mode_graphic1,
        &v99x8_device::default_border,
        &v99x8_device::sprite_mode1,
        &v99x8_device::default_draw_sprite
    },
    { 0x04,
        &v99x8_device::mode_graphic23,
        &v99x8_device::default_border,
        &v99x8_device::sprite_mode1,
        &v99x8_device::default_draw_sprite
    },
    { 0x08,
        &v99x8_device::mode_graphic23,
        &v99x8_device::default_border,
        &v99x8_device::sprite_mode2,
        &v99x8_device::default_draw_sprite
    },
    { 0x0c,
        &v99x8_device::mode_graphic4,
        &v99x8_device::default_border,
        &v99x8_device::sprite_mode2,
        &v99x8_device::default_draw_sprite
    },
    { 0x10,
        &v99x8_device::mode_graphic5,
        &v99x8_device::graphic5_border,
        &v99x8_device::sprite_mode2,
        &v99x8_device::graphic5_draw_sprite
    },
    { 0x14,
        &v99x8_device::mode_graphic6,
        &v99x8_device::default_border,
        &v99x8_device::sprite_mode2,
        &v99x8_device::default_draw_sprite
    },
    { 0x1c,
        &v99x8_device::mode_graphic7,
        &v99x8_device::graphic7_border,
        &v99x8_device::sprite_mode2,
        &v99x8_device::graphic7_draw_sprite
    },
    { 0x0a,
        &v99x8_device::mode_text2,
        &v99x8_device::default_border,
        nullptr,
        nullptr
    },
    { 0xff,
        &v99x8_device::mode_unknown,
        &v99x8_device::default_border,
        nullptr,
        nullptr
    }
};

// =====================================================================
//  Implementation
//
//  Everything below is the MAME 0.289 v9938.cpp body in original order.
//  Each PORT: comment names the framework call it replaced.  The sections
//  the card owns in phase 1 are stubbed with the phase that restores them.
// =====================================================================

// ---------------------------------------------------------------------
//  Construction / reset
// ---------------------------------------------------------------------

v99x8_device::v99x8_device(int model, uint32_t vram_size)
:   m_offset_x(0),
	m_offset_y(0),
	m_visible_y(0),
	m_mode(0),
	m_pal_write_first(0),
	m_cmd_write_first(0),
	m_pal_write(0),
	m_cmd_write(0),
	m_read_ahead(0),
	m_v9958_sp_mode(0),
	m_second_field(0),
	m_address_latch(0),
	m_vram_size(int(vram_size)),
	m_vram(VRAM_TOTAL_SIZE, 0),
	m_int_state(0),
	m_scanline(0),
	m_blink(0),
	m_blink_count(0),
	m_mx_delta(0),
	m_my_delta(0),
	m_button_state(0),
	m_pal_ntsc(0),
	m_scanline_start(0),
	m_vblank_start(0),
	m_scanline_max(0),
	m_height(0),
	m_model(model)
{
	// PORT: device_palette_interface() and device_video_interface() are gone.
	// PORT: screen().register_screen_bitmap(m_bitmap) is gone; the render
	// target is a plain buffer sized for the tallest (PAL) frame.
	m_bitmap.assign(size_t(surface_max_height) * surface_width, 0);
	m_scratch_line.assign(V9938_LONG_WIDTH, 0);
	m_frame.assign(size_t(surface_width) * surface_max_height * 4u, 0);
}

// PORT: MAME's v9938_device()/v9958_device() constructors and
// DEFINE_DEVICE_TYPE are gone; the card selects the model, and device
// discovery is the engine's card ABI.
// PORT: memory_space_config() and memmap() are gone; the VRAM address space
// became m_vram[] (global_mask 0x3ffff, all RAM).

void v99x8_device::device_start()
{
	// PORT: MAME allocated VRAM through its address space and armed
	// m_line_timer here.  m_vram is allocated in the constructor; the card
	// owns the scanline timer and calls line_tick().
	if (m_vram_size < 0x20000)
	{
		// set unavailable RAM to 0xff
		for (int addr = m_vram_size; addr < VRAM_TOTAL_SIZE; addr++) m_vram[addr] = 0xff;
	}

	// PORT: timer_alloc(FUNC(v99x8_device::update_line), this) is the card's
	// scheduled scanline event.

	palette_init();
	device_reset();
}

void v99x8_device::device_reset()
{
	m_mmc = {};
	m_vdp_ops_count = 0;
	m_vdp_engine = nullptr;
	int i;

	// offset reset
	m_offset_x = 8;
	m_offset_y = 0;
	m_visible_y = 192;
	// register reset
	reset_palette (); // palette registers
	for (i=0;i<10;i++) m_stat_reg[i] = 0;
	m_stat_reg[2] = 0x0c;
	if (m_model == MODEL_V9958) m_stat_reg[1] |= 4;
	for (i=0;i<48;i++) m_cont_reg[i] = 0;
	m_cmd_write_first = m_pal_write_first = 0;
	m_cmd_write = m_pal_write = 0;
	irq_line(0);
	m_int_state = 0;
	m_read_ahead = 0; m_address_latch = 0; // ???
	// FIXME: this drifts the scanline number wrt screen h/vpos
	m_scanline = 0;
	// MZ: The status registers 4 and 6 hold the high bits of the sprite
	// collision location. The unused bits are set to 1.
	// SR3: x x x x x x x x
	// SR4: 1 1 1 1 1 1 1 x
	// SR5: y y y y y y y y
	// SR6: 1 1 1 1 1 1 y y
	// Note that status register 4 is used in detection algorithms to tell
	// apart the tms9929 from the v99x8.

	// TODO: SR3-S6 do not yet store the information about the sprite collision
	m_stat_reg[4] = 0xfe;
	m_stat_reg[6] = 0xfc;

	// PORT: m_line_timer->adjust(attotime::from_ticks(HTOTAL*2, m_clock), 0, ...)
	// is the card's scanline event; the card re-arms it after this returns.
	m_blink = 0;
	m_blink_count = 0;
	m_mx_delta = 0;
	m_my_delta = 0;
	m_button_state = 0;
	m_second_field = 0;
	m_pal_ntsc = 0;
	m_v9958_sp_mode = 0;
	m_offset_x = 8;
	m_mode = V9938_MODE_TEXT1;

	update_display_parameters();

	// PORT: device_reset() also reset the palette through the palette
	// interface; reset_palette() above is the port's equivalent.
}

void v99x8_device::reset_palette()
{
	// taken from V9938 Technical Data book, page 148. it's in G-R-B format
	static const uint8_t pal16[16*3] = {
		0, 0, 0, // 0: black/transparent
		0, 0, 0, // 1: black
		6, 1, 1, // 2: medium green
		7, 3, 3, // 3: light green
		1, 1, 7, // 4: dark blue
		3, 2, 7, // 5: light blue
		1, 5, 1, // 6: dark red
		6, 2, 7, // 7: cyan
		1, 7, 1, // 8: medium red
		3, 7, 3, // 9: light red
		6, 6, 1, // 10: dark yellow
		6, 6, 4, // 11: light yellow
		4, 1, 1, // 12: dark green
		2, 6, 5, // 13: magenta
		5, 5, 5, // 14: gray
		7, 7, 7  // 15: white
	};
	int i, red;

	for (i=0;i<16;i++)
	{
		// set the palette registers
		m_pal_reg[i*2+0] = pal16[i*3+1] << 4 | pal16[i*3+2];
		m_pal_reg[i*2+1] = pal16[i*3];
		// set the reference table
		set_pen16(i, rgb3(pal16[i*3+1], pal16[i*3], pal16[i*3+2]));
	}

	// set internal palette GRAPHIC 7
	for (i=0;i<256;i++)
	{
		red = (i << 1) & 6; if (red == 6) red++;

		set_pen256(i, rgb3((i >> 2) & 7, (i >> 5) & 7, uint8_t(red)));
	}
}

// ---------------------------------------------------------------------
//  Ports, registers, raster, mode select
// ---------------------------------------------------------------------

// =====================================================================
//  Palette interface
// =====================================================================

void v99x8_device::palette_w(uint8_t data)
{
	int indexp;

	if (m_pal_write_first)
	{
		// store in register
		indexp = m_cont_reg[0x10] & 15;
		m_pal_reg[indexp*2] = m_pal_write & 0x77;
		m_pal_reg[indexp*2+1] = data & 0x07;

		// update palette
		set_pen16(indexp, rgb3((m_pal_write >> 4) & 7, data & 7, m_pal_write & 7));

		m_cont_reg[0x10] = (m_cont_reg[0x10] + 1) & 15;
		m_pal_write_first = 0;
	}
	else
	{
		m_pal_write = data;
		m_pal_write_first = 1;
	}
}

// PORT: v99x8_device::s_pal_indYJK[0x20000], the V9958's 17-bit YJK
// reference table built by v9958_device::palette_init() in v9938.cpp:355-378.
// MAME built it for every V9958 instance at device start.  Here it is one
// process-wide table built on first use, so a V9938 (or a V9958 that never
// renders YJK/YAE) never pays the 512 KiB and the one-time loop.
const std::vector<uint32_t> &yjk_palette()
{
	static const std::vector<uint32_t> table = [] {
		std::vector<uint32_t> colors(0x20000, 0);
		for (int y = 0; y < 32; y++)
			for (int k = 0; k < 64; k++)
				for (int j = 0; j < 64; j++)
				{
					// calculate the color
					const int k0 = (k >= 32) ? (k - 64) : k;
					const int j0 = (j >= 32) ? (j - 64) : j;
					int r = y + j0;
					int b = (y * 5 - 2 * j0 - k0) / 4;
					int g = y + k0;
					if (r < 0) r = 0; else if (r > 31) r = 31;
					if (g < 0) g = 0; else if (g > 31) g = 31;
					if (b < 0) b = 0; else if (b > 31) b = 31;

					colors[y | j << 5 | k << (5 + 6)] = rgb5(uint8_t(r), uint8_t(g), uint8_t(b));
				}
		return colors;
	}();
	return table;
}

// PORT: v9938_device::palette_init() added nothing; v9958_device's built the
// YJK table.  Asking for the table here only warms it for the one model that
// can render from it, and a card that overrides this keeps control.
void v99x8_device::palette_init()
{
	if (m_model == MODEL_V9958)
		(void)yjk_palette();
}

// =====================================================================
//  Raster: MAME's update_line() TIMER_CALLBACK_MEMBER
// =====================================================================

void v99x8_device::line_tick()
{
	update_line();
}

void v99x8_device::update_line()
{
	int scanline = (m_scanline - (m_scanline_start + m_offset_y));

	update_command();

	// set flags
	if (m_scanline == (m_scanline_start + m_offset_y))
	{
		m_stat_reg[2] &= ~0x40;
	}
	else if (m_scanline == (m_scanline_start + m_offset_y + m_visible_y))
	{
		m_stat_reg[2] |= 0x40;
		m_stat_reg[0] |= 0x80;
	}

	if ( (scanline >= 0) && (scanline <= m_scanline_max) &&
		(((scanline + m_cont_reg[23]) & 255) == m_cont_reg[19]) )
	{
		m_stat_reg[1] |= 1;
		LOGMASKED(LOG_INT, "Scanline interrupt (%d)\n", scanline);
	}
	else if (!(m_cont_reg[0] & 0x10))
	{
		m_stat_reg[1] &= 0xfe;
	}

	check_int();

	// check for start of vblank
	if (m_scanline == m_vblank_start)
	{
		interrupt_start_vblank();
	}

	// render the current line
	if (m_scanline < m_vblank_start)
	{
		refresh_line(scanline);
	}

	if (++m_scanline >= m_height)
	{
		m_scanline = 0;
		// PAL/NTSC changed?
		int pal = m_cont_reg[9] & 2;
		if (m_pal_ntsc != pal)
		{
			m_pal_ntsc = pal;
			update_display_parameters();
		}
		//screen().reset_origin();
		m_offset_y = position_offset(m_cont_reg[18] >> 4);
		// PORT: set_screen_parameters() folded into update_display_parameters().
		update_display_parameters();
	}
}

// PORT: MAME's set_screen_parameters() and configure_pal_ntsc().  The screen
// device's set_raw()/configure() became buffer geometry plus the vblank
// boundary the card uses to drive the engine's IRQ signal.
void v99x8_device::update_display_parameters()
{
	if (m_pal_ntsc)
	{
		// PAL
		m_scanline_start = (m_cont_reg[9] & 0x80) ? 43 : 53;
		m_scanline_max = 255;
	}
	else
	{
		// NTSC
		m_scanline_start = (m_cont_reg[9] & 0x80) ? 16 : 26;
		m_scanline_max = (m_cont_reg[9] & 0x80) ? 234 : 244;
	}
	m_visible_y = (m_cont_reg[9] & 0x80) ? 212 : 192;

	// PORT: configure_pal_ntsc()'s geometry half.
	m_height = m_pal_ntsc ? VTOTAL_PAL : VTOTAL_NTSC;
	m_vblank_start = m_height - VERTICAL_SYNC - TOP_ERASE; /* Sync + top erase */

	// PORT: the extra / 2 that refreshes MAME's screen at the pixel clock.
	// The engine has no screen device, so the card's scanline event is the
	// only clock and this factor is not needed here.
}

/*
    Colorbus inputs
    vdp will process mouse deltas only if it is in mouse mode
    Reg 8: MS LP x x x x x x
*/
void v99x8_device::colorbus_x_input(int mx_delta)
{
	if ((m_cont_reg[8] & 0xc0) == 0x80)
	{
		m_mx_delta += mx_delta;
		if (m_mx_delta < -127) m_mx_delta = -127;
		if (m_mx_delta > 127) m_mx_delta = 127;
	}
}

void v99x8_device::colorbus_y_input(int my_delta)
{
	if ((m_cont_reg[8] & 0xc0) == 0x80)
	{
		m_my_delta += my_delta;
		if (m_my_delta < -127) m_my_delta = -127;
		if (m_my_delta > 127) m_my_delta = 127;
	}
}

void v99x8_device::colorbus_button_input(bool switch1_pressed, bool switch2_pressed)
{
	// save button state
	m_button_state = (switch2_pressed? 0x80 : 0x00) | (switch1_pressed? 0x40 : 0x00);
}

// =====================================================================
//  CPU port interface
// =====================================================================

// PORT: screen_update(screen_device&, bitmap_rgb32&, const rectangle&) is
// gone.  Its copybitmap() role is convert_line(), which the card reads through
// framebuffer().

uint8_t v99x8_device::read(uint32_t offset)
{
	switch (offset & 3)
	{
	case 0: return vram_r();
	case 1: return status_r();
	}
	return 0xff;
}

void v99x8_device::write(uint32_t offset, uint8_t data)
{
	switch (offset & 3)
	{
	case 0: vram_w(data);       break;
	case 1: command_w(data);    break;
	case 2: palette_w(data);    break;
	case 3: register_w(data);   break;
	}
}

uint8_t v99x8_device::vram_r()
{
	uint8_t ret;
	int address;

	address = ((int)m_cont_reg[14] << 14) | m_address_latch;

	m_cmd_write_first = 0;

	ret = m_read_ahead;

	if (m_cont_reg[45] & 0x40)  // Expansion memory
	{
		if ( (m_mode == V9938_MODE_GRAPHIC6) || (m_mode == V9938_MODE_GRAPHIC7) )
			address >>= 1;  // correct?
		// Expansion memory only offers 64 K
		if (m_vram_size > 0x20000 && ((address & 0x10000)==0))
			m_read_ahead = m_vram[EXPMEM_OFFSET + address];
		else
			m_read_ahead = 0xff;
	}
	else
	{
		m_read_ahead = vram_read(address);
	}

	m_address_latch = (m_address_latch + 1) & 0x3fff;
	if ((!m_address_latch) && (m_cont_reg[0] & 0x0c) ) // correct ???
	{
		m_cont_reg[14] = (m_cont_reg[14] + 1) & 7;
	}

	return ret;
}

uint8_t v99x8_device::status_r()
{
	int reg;
	uint8_t ret;

	m_cmd_write_first = 0;

	reg = m_cont_reg[15] & 0x0f;
	if (reg > 9)
		return 0xff;

	switch (reg)
	{
	case 0:
		ret = m_stat_reg[0];
		m_stat_reg[0] &= 0x1f;
		break;
	case 1:
		ret = m_stat_reg[1];
		m_stat_reg[1] &= 0xfe;
		// mouse mode: add button state
		if ((m_cont_reg[8] & 0xc0) == 0x80)
			ret |= m_button_state & 0xc0;
		break;
	case 2:
		/*update_command ();*/
		/*
		WTF is this? Whatever this was intended to do, it is nonsensical.
		Might as well pick a random number....
		This was an attempt to emulate H-Blank flag ;)
		n = cycles_currently_ran ();
		if ( (n < 28) || (n > 199) ) vdp.statReg[2] |= 0x20;
		else vdp.statReg[2] &= ~0x20;
		*/
		// PORT: machine().rand() became the card's deterministic rng().
		if (rng() & 1) m_stat_reg[2] |= 0x20;
		else m_stat_reg[2] &= ~0x20;
		ret = m_stat_reg[2];
		break;
	case 3:
		if ((m_cont_reg[8] & 0xc0) == 0x80)
		{   // mouse mode: return x mouse delta
			ret = m_mx_delta;
			m_mx_delta = 0;
		}
		else
			ret = m_stat_reg[3];
		break;
	case 5:
		if ((m_cont_reg[8] & 0xc0) == 0x80)
		{   // mouse mode: return y mouse delta
			ret = m_my_delta;
			m_my_delta = 0;
		}
		else
			ret = m_stat_reg[5];
		break;
	case 7:
		ret = m_stat_reg[7];
		m_stat_reg[7] = m_cont_reg[44] = vdp_to_cpu () ;
		break;
	default:
		ret = m_stat_reg[reg];
		break;
	}

	LOGMASKED(LOG_STATUS, "Read %02x from S#%d\n", ret, reg);
	check_int ();

	return ret;
}

void v99x8_device::vram_w(uint8_t data)
{
	int address;

	/*update_command ();*/

	m_cmd_write_first = 0;

	address = ((int)m_cont_reg[14] << 14) | m_address_latch;

	if (m_cont_reg[45] & 0x40)
	{
		if ( (m_mode == V9938_MODE_GRAPHIC6) || (m_mode == V9938_MODE_GRAPHIC7) )
			address >>= 1;  // correct?
		if (m_vram_size > 0x20000 && ((address & 0x10000)==0))
			m_vram[EXPMEM_OFFSET + address] = data;
	}
	else
	{
		vram_write(address, data);
	}

	m_address_latch = (m_address_latch + 1) & 0x3fff;
	if ((!m_address_latch) && (m_cont_reg[0] & 0x0c) ) // correct ???
	{
		m_cont_reg[14] = (m_cont_reg[14] + 1) & 7;
	}
}

void v99x8_device::command_w(uint8_t data)
{
	if (m_cmd_write_first)
	{
		if (data & 0x80)
		{
			if (!(data & 0x40))
				register_write (data & 0x3f, m_cmd_write);
		}
		else
		{
			m_address_latch =
			(((uint16_t)data << 8) | m_cmd_write) & 0x3fff;
			if ( !(data & 0x40) ) vram_r (); // read ahead!
		}

		m_cmd_write_first = 0;
	}
	else
	{
		m_cmd_write = data;
		m_cmd_write_first = 1;
	}
}

void v99x8_device::register_w(uint8_t data)
{
	int reg;

	reg = m_cont_reg[17] & 0x3f;
	if (reg != 17)
		register_write(reg, data); // true ?

	if (!(m_cont_reg[17] & 0x80))
		m_cont_reg[17] = (m_cont_reg[17] + 1) & 0x3f;
}

// =====================================================================
//  Memory functions
// =====================================================================

// PORT: vram_read/vram_write keep MAME's GRAPHIC 6/7 address interleave and
// return 0xff outside the fitted VRAM, which is how the address space's
// global_mask plus the device_start() pad behaved.

void v99x8_device::vram_write(int offset, int data)
{
	int newoffset;

	if ( (m_mode == V9938_MODE_GRAPHIC6) || (m_mode == V9938_MODE_GRAPHIC7) )
	{
		newoffset = ((offset & 1) << 16) | (offset >> 1);
		if (newoffset < m_vram_size)
			m_vram[newoffset] = uint8_t(data);
	}
	else
	{
		if (offset < m_vram_size)
			m_vram[offset] = uint8_t(data);
	}
}

int v99x8_device::vram_read(int offset)
{
	if ( (m_mode == V9938_MODE_GRAPHIC6) || (m_mode == V9938_MODE_GRAPHIC7) )
		return vram_barrier(((offset & 1) << 16) | (offset >> 1));
	else
		return vram_barrier(offset);
}

// PORT: the renderers and command unit index VRAM directly.
// MAME relied on the address space's ability to return 0xff past the fitted
// memory; an offset mask is not enough because VRAM sizes are not powers of
// two, so both accessors clamp against m_vram_size the same way vram_read()
// does.
uint8_t v99x8_device::vram_barrier(int offset) const
{
	if (offset < 0 || offset >= m_vram_size)
		return 0xff;
	return m_vram[offset];
}

void v99x8_device::vram_barrier_w(int offset, uint8_t data)
{
	if (offset >= 0 && offset < m_vram_size)
		m_vram[offset] = data;
}

void v99x8_device::check_int()
{
	uint8_t n;

	n = ( (m_cont_reg[1] & 0x20) && (m_stat_reg[0] & 0x80) /*&& m_vblank_int*/) ||
	( (m_stat_reg[1] & 0x01) && (m_cont_reg[0] & 0x10) );

	#if 0
	if(n && m_vblank_int)
	{
		m_vblank_int = 0;
	}
	#endif

	if (n != m_int_state)
	{
		m_int_state = n;
		LOGMASKED(LOG_INT, "IRQ line %s\n", n ? "up" : "down");
	}

	/*
	** Somehow the IRQ request is going down without cpu_irq_line () being
	** called; because of this Mr. Ghost, Xevious and SD Snatcher don't
	** run. As a patch it's called every scanline
	*/
	// FIXME: breaks nichibutsu hrdvd.cpp & nichild.cpp, really needs INPUT_MERGER instead.
	// PORT: m_int_callback(n) became the card's signal drive, which only
	// changes the engine's IRQ signal when the level actually differs.
	irq_line(n);
}

// =====================================================================
//  Register functions
// =====================================================================

void v99x8_device::register_write (int reg, int data)
{
	static uint8_t const reg_mask[] =
	{
		0x7e, 0x7b, 0x7f, 0xff, 0x3f, 0xff, 0x3f, 0xff,
		0xfb, 0xbf, 0x07, 0x03, 0xff, 0xff, 0x07, 0x0f,
		0x0f, 0xbf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x7f, 0x3f, 0x07
	};

	if (reg <= 27)
	{
		data &= reg_mask[reg];
		if (m_cont_reg[reg] == data)
			return;
	}

	if (reg > 46)
	{
		LOGMASKED(LOG_WARN, "Attempted to write to non-existent R#%d\n", reg);
		return;
	}

	/*update_command ();*/

	switch (reg) {
		// registers that affect interrupt and display mode
	case 0:
	case 1:
		m_cont_reg[reg] = data;
		set_mode();
		check_int();
		LOGMASKED(LOG_MODE, "Mode = %s\n", v9938_modes[m_mode]);
		break;

	case 18:
	case 9:
		m_cont_reg[reg] = data;
		// recalc offset
		m_offset_x = 8 + position_offset(m_cont_reg[18] & 0x0f);
		// Y offset is only applied once per frame?
		break;

	case 15:
		m_pal_write_first = 0;
		break;

		// color burst registers aren't emulated
	case 20:
	case 21:
	case 22:
		LOGMASKED(LOG_NOTIMP, "Write %02xh to R#%d; color burst not emulated\n", data, reg);
		break;
	case 25:
	case 26:
	case 27:
		if (m_model != MODEL_V9958)
		{
			LOGMASKED(LOG_WARN, "Attempting to write %02xh to R#%d (invalid on v9938)\n", data, reg);
			data = 0;
		}
		else
		{
			if(reg == 25)
				m_v9958_sp_mode = data & 0x18;
		}
		break;

	case 44:
		cpu_to_vdp (data);
		break;

	case 46:
		command_unit_w (data);
		break;
	}

	if (reg != 15)
		LOGMASKED(LOG_REGWRITE, "Write %02x to R#%d\n", data, reg);

	m_cont_reg[reg] = data;
}

// =====================================================================
//  Refresh / render functions
// =====================================================================

inline bool v99x8_device::v9938_second_field()
{
	// MAME derived this from the screen device's current field.  The port
	// tracks the phase m_stat_reg[2] bit 1 alternates at every vblank.
	return (m_stat_reg[2] >> 1) & 1;
}

// PORT: m_bitmap.pix(y) is bitmap_line(y); ln2 and ln still receive 32-bit
// values in MAME's 0x00RRGGBB order.  They are scratch rows now, converted
// once per line by convert_line().

void v99x8_device::refresh_32(int line)
{
	bool double_lines = false;
	uint8_t col[256];
	uint32_t *ln, *ln2 = nullptr;

	if (m_cont_reg[9] & 0x08)
	{
		ln = bitmap_line(m_scanline*2+((m_stat_reg[2]>>1)&1));
	}
	else
	{
		ln = bitmap_line(m_scanline*2);
		ln2 = bitmap_line(m_scanline*2+1);
		double_lines = true;
	}

	if ( !(m_cont_reg[1] & 0x40) || (m_stat_reg[2] & 0x40) )
	{
		(this->*s_modes[m_mode].border_32)(ln);
	}
	else
	{
		(this->*s_modes[m_mode].visible_32)(ln, line);
		if (s_modes[m_mode].sprites)
		{
			(this->*s_modes[m_mode].sprites)(line, col);
			(this->*s_modes[m_mode].draw_sprite_32)(ln, col);
		}
	}

	if (double_lines)
		memcpy(ln2, ln, (512 + 32) * sizeof(*ln));

}

void v99x8_device::refresh_line(int line)
{
	pen_t ind16, ind256;

	ind16 = pen16(0);
	ind256 = pen256(0);

	if ( !(m_cont_reg[8] & 0x20) && (m_mode != V9938_MODE_GRAPHIC5) )
	{
		set_pen16(0, pen16(m_cont_reg[7] & 0x0f));
		set_pen256(0, pen256(m_cont_reg[7]));
	}

	refresh_32(line);

	if ( !(m_cont_reg[8] & 0x20) && (m_mode != V9938_MODE_GRAPHIC5) )
	{
		set_pen16(0, ind16);
		set_pen256(0, ind256);
	}

	// PORT: MAME's screen update copied m_bitmap into the output bitmap
	// later.  The port converts the rows this call just produced straight
	// into the engine's RGBA8 surface.
	convert_line(line);
}

uint32_t *v99x8_device::bitmap_line(int y)
{
	if (y < 0) y = 0;
	if (y >= int(surface_max_height)) y = int(surface_max_height) - 1;
	return &m_bitmap[size_t(y) * surface_width];
}

// PORT: MAME's copybitmap(bitmap, m_bitmap, ...).  MAME's bitmap_rgb32 stores
// 0xAARRGGBB in native uint32_t order, which on a little-endian host is the
// byte sequence B,G,R,A; the engine surface is SRH_VIDEO_RGBA8, byte sequence
// R,G,B,A.  So this is a byte swap with no resampling: the pen values are
// already 8-bit (rgb3 expanded the 3-bit V9938 components once) and taking
// them through a 5-bit round trip here would quantise them again.
void v99x8_device::convert_line(int line)
{
	// PORT: convert the row refresh_32 actually wrote in this field.
	const int row = m_scanline * 2 + ((m_cont_reg[9] & 0x08) ? ((m_stat_reg[2] >> 1) & 1) : 0);
	const uint32_t *src = bitmap_line(row);
	uint8_t *dst = m_frame.data() + size_t(row) * surface_width * 4u;

	for (uint32_t x = 0; x < surface_width; ++x)
	{
		const uint32_t rgb = src[x];
		*dst++ = uint8_t(rgb >> 16);   // R
		*dst++ = uint8_t(rgb >> 8);    // G
		*dst++ = uint8_t(rgb);         // B
		*dst++ = 0xff;                 // A: the surface is opaque
	}

	if (!(m_cont_reg[9] & 0x08))
	{
		memcpy(m_frame.data() + size_t(row + 1) * surface_width * 4u,
		       m_frame.data() + size_t(row) * surface_width * 4u,
		       surface_width * 4u);
	}
	(void)line;
}

// =====================================================================
//  Vblank / blink
// =====================================================================

void v99x8_device::interrupt_start_vblank()
{
	// at every frame, vdp switches fields
	m_stat_reg[2] = (m_stat_reg[2] & 0xfd) | (~m_stat_reg[2] & 2);

	// color blinking
	if (!(m_cont_reg[13] & 0xf0))
		m_blink = 0;
	else if (!(m_cont_reg[13] & 0x0f))
		m_blink = 1;
	else
	{
		// both on and off counter are non-zero: timed blinking
		if (m_blink_count)
			m_blink_count--;
		if (!m_blink_count)
		{
			m_blink = !m_blink;
			if (m_blink)
				m_blink_count = (m_cont_reg[13] >> 4) * 10;
			else
				m_blink_count = (m_cont_reg[13] & 0x0f) * 10;
		}
	}
}

// =====================================================================
//  Mode select
// =====================================================================

void v99x8_device::set_mode()
{
	int n,i;

	n = (((m_cont_reg[0] & 0x0e) << 1) | ((m_cont_reg[1] & 0x18) >> 3));
	for (i=0;;i++)
	{
		if ( (s_modes[i].m == n) || (s_modes[i].m == 0xff) ) break;
	}

	// MZ: What happens when the mode is changed during command execution?
	// This is left unspecified in the docs. On a Geneve, experiments showed
	// that the command is not aborted (the CE flag is still 1) and runs for
	// about 90% of the nominal execution time, but VRAM is only correctly
	// filled up to the time of switching, and after that, isolated locations
	// within the normally affected area are changed, but inconsistently.
	// Obviously, it depends on the time when the switch happened.
	// This behavior occurs on every switch from a mode Graphics4 and higher
	// to another mode, e.g. also from Graphics7 to Graphics6.
	// Due to the lack of more information, we simply abort the command.

	if (m_mode != i)
	{
		m_vdp_engine = nullptr;
		m_stat_reg[2] &= 0xFE;
	}

	m_mode = i;
}

// PORT: MAME command loops use the card-owned VRAM barriers.
/*************************************************************/
/** Completely rewritten by Alex Wulms:                     **/
/**  - VDP Command execution 'in parallel' with CPU         **/
/**  - Corrected behaviour of VDP commands                  **/
/**  - Made it easier to implement correct S7/8 mapping     **/
/**    by concentrating VRAM access in one single place     **/
/**  - Made use of the 'in parallel' VDP command exec       **/
/**    and correct timing. You must call the function       **/
/**    LoopVDP() from LoopZ80 in MSX.c. You must call it    **/
/**    exactly 256 times per screen refresh.                **/
/** Started on       : 11-11-1999                           **/
/** Beta release 1 on:  9-12-1999                           **/
/** Beta release 2 on: 20-01-2000                           **/
/**  - Corrected behaviour of VRM <-> Z80 transfer          **/
/**  - Improved performance of the code                     **/
/** Public release 1.0: 20-04-2000                          **/
/*************************************************************/

#define VDP_VRMP5(MX, X, Y) ((!MX) ? (((Y&1023)<<7) + ((X&255)>>1)) : (EXPMEM_OFFSET + ((Y&511)<<7) + ((X&255)>>1)))
#define VDP_VRMP6(MX, X, Y) ((!MX) ? (((Y&1023)<<7) + ((X&511)>>2)) : (EXPMEM_OFFSET + ((Y&511)<<7) + ((X&511)>>2)))
//#define VDP_VRMP7(MX, X, Y) ((!MX) ? (((Y&511)<<8) + ((X&511)>>1)) : (EXPMEM_OFFSET + ((Y&255)<<8) + ((X&511)>>1)))
#define VDP_VRMP7(MX, X, Y) ((!MX) ? (((X&2)<<15) + ((Y&511)<<7) + ((X&511)>>2)) : (EXPMEM_OFFSET + ((Y&511)<<7) + ((X&511)>>2))/*(EXPMEM_OFFSET + ((Y&255)<<8) + ((X&511)>>1))*/)
//#define VDP_VRMP8(MX, X, Y) ((!MX) ? (((Y&511)<<8) + (X&255)) : (EXPMEM_OFFSET + ((Y&255)<<8) + (X&255)))
#define VDP_VRMP8(MX, X, Y) ((!MX) ? (((X&1)<<16) + ((Y&511)<<7) + ((X>>1)&127)) : (EXPMEM_OFFSET + ((Y&511)<<7) + ((X>>1)&127))/*(EXPMEM_OFFSET + ((Y&255)<<8) + (X&255))*/)

#define VDP_VRMP(M, MX, X, Y) VDPVRMP(M, MX, X, Y)
#define VDP_POINT(M, MX, X, Y) VDPpoint(M, MX, X, Y)
#define VDP_PSET(M, MX, X, Y, C, O) VDPpset(M, MX, X, Y, C, O)

#define CM_ABRT  0x0
#define CM_POINT 0x4
#define CM_PSET  0x5
#define CM_SRCH  0x6
#define CM_LINE  0x7
#define CM_LMMV  0x8
#define CM_LMMM  0x9
#define CM_LMCM  0xA
#define CM_LMMC  0xB
#define CM_HMMV  0xC
#define CM_HMMM  0xD
#define CM_YMMM  0xE
#define CM_HMMC  0xF

/*************************************************************
Many VDP commands are executed in some kind of loop but
essentially, there are only a few basic loop structures
that are re-used. We define the loop structures that are
re-used here so that they have to be entered only once
*************************************************************/
#define pre_loop \
while ((cnt-=delta) > 0) {
	#define post_loop \
}

// Loop over DX, DY
#define post__x_y(MX) \
if (!--ANX || ((ADX+=TX)&MX)) { \
	if (!(--NY&1023) || (DY+=TY)==-1) \
		break; \
	else { \
		ADX=DX; \
		ANX=NX; \
	} \
} \
post_loop

// Loop over DX, SY, DY
#define post__xyy(MX) \
if ((ADX+=TX)&MX) { \
	if (!(--NY&1023) || (SY+=TY)==-1 || (DY+=TY)==-1) \
		break; \
	else \
		ADX=DX; \
} \
post_loop

// Loop over SX, DX, SY, DY
#define post_xxyy(MX) \
if (!--ANX || ((ASX+=TX)&MX) || ((ADX+=TX)&MX)) { \
	if (!(--NY&1023) || (SY+=TY)==-1 || (DY+=TY)==-1) \
		break; \
	else { \
		ASX=SX; \
		ADX=DX; \
		ANX=NX; \
	} \
} \
post_loop

/*************************************************************/
/** Variables visible only in this module                   **/
/*************************************************************/
static const uint8_t Mask[4] = { 0x0F,0x03,0x0F,0xFF };
static const int  PPB[4]  = { 2,4,2,1 };
static const int  PPL[4]  = { 256,512,512,256 };

//  SprOn SprOn SprOf SprOf
//  ScrOf ScrOn ScrOf ScrOn
static const int srch_timing[8]={
	818, 1025,  818,  830, // ntsc
	696,  854,  696,  684  // pal
};
static const int line_timing[8]={
	1063, 1259, 1063, 1161,
	904,  1026, 904,  953
};
static const int hmmv_timing[8]={
	439,  549,  439,  531,
	366,  439,  366,  427
};
static const int lmmv_timing[8]={
	873,  1135, 873, 1056,
	732,  909,  732,  854
};
static const int ymmm_timing[8]={
	586,  952,  586,  610,
	488,  720,  488,  500
};
static const int hmmm_timing[8]={
	818,  1111, 818,  854,
	684,  879,  684,  708
};
static const int lmmm_timing[8]={
	1160, 1599, 1160, 1172,
	964,  1257, 964,  977
};

/** VDPVRMP() **********************************************/
/** Calculate addr of a pixel in vram                       **/
/*************************************************************/
inline int v99x8_device::VDPVRMP(uint8_t M,int MX,int X,int Y)
{
	switch(M)
	{
	case 0: return VDP_VRMP5(MX,X,Y);
	case 1: return VDP_VRMP6(MX,X,Y);
	case 2: return VDP_VRMP7(MX,X,Y);
	case 3: return VDP_VRMP8(MX,X,Y);
	}

	return 0;
}

/** VDPpoint5() ***********************************************/
/** Get a pixel on screen 5                                 **/
/*************************************************************/
inline uint8_t v99x8_device::VDPpoint5(int MXS, int SX, int SY)
{
	return (vram_barrier(VDP_VRMP5(MXS, SX, SY)) >>
		(((~SX)&1)<<2)
		)&15;
}

/** VDPpoint6() ***********************************************/
/** Get a pixel on screen 6                                 **/
/*************************************************************/
inline uint8_t v99x8_device::VDPpoint6(int MXS, int SX, int SY)
{
	return (vram_barrier(VDP_VRMP6(MXS, SX, SY)) >>
		(((~SX)&3)<<1)
		)&3;
}

/** VDPpoint7() ***********************************************/
/** Get a pixel on screen 7                                 **/
/*************************************************************/
inline uint8_t v99x8_device::VDPpoint7(int MXS, int SX, int SY)
{
	return (vram_barrier(VDP_VRMP7(MXS, SX, SY)) >>
		(((~SX)&1)<<2)
		)&15;
}

/** VDPpoint8() ***********************************************/
/** Get a pixel on screen 8                                 **/
/*************************************************************/
inline uint8_t v99x8_device::VDPpoint8(int MXS, int SX, int SY)
{
	return vram_barrier(VDP_VRMP8(MXS, SX, SY));
}

/** VDPpoint() ************************************************/
/** Get a pixel on a screen                                 **/
/*************************************************************/
inline uint8_t v99x8_device::VDPpoint(uint8_t SM, int MXS, int SX, int SY)
{
	switch(SM)
	{
	case 0: return VDPpoint5(MXS,SX,SY);
	case 1: return VDPpoint6(MXS,SX,SY);
	case 2: return VDPpoint7(MXS,SX,SY);
	case 3: return VDPpoint8(MXS,SX,SY);
	}

	return(0);
}

/** VDPpsetlowlevel() ****************************************/
/** Low level function to set a pixel on a screen           **/
/** Make it inline to make it fast                          **/
/*************************************************************/
inline void v99x8_device::VDPpsetlowlevel(int addr, uint8_t CL, uint8_t M, uint8_t OP)
{
	// If this turns out to be too slow, get a pointer to the address space
	// and work directly on it.
	uint8_t val = vram_barrier(addr);
	switch (OP)
	{
	case 0: val = (val & M) | CL; break;
	case 1: val = val & (CL | M); break;
	case 2: val |= CL; break;
	case 3: val ^= CL; break;
	case 4: val = (val & M) | ~(CL | M); break;
	case 8: if (CL) val = (val & M) | CL; break;
	case 9: if (CL) val = val & (CL | M); break;
	case 10: if (CL) val |= CL; break;
	case 11:  if (CL) val ^= CL; break;
	case 12:  if (CL) val = (val & M) | ~(CL|M); break;
	default:
		LOGMASKED(LOG_WARN, "Invalid operation %d in pset\n", OP);
	}

	vram_barrier_w(addr, val);
}

/** VDPpset5() ***********************************************/
/** Set a pixel on screen 5                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset5(int MXD, int DX, int DY, uint8_t CL, uint8_t OP)
{
	uint8_t SH = ((~DX)&1)<<2;
	VDPpsetlowlevel(VDP_VRMP5(MXD, DX, DY), CL << SH, ~(15<<SH), OP);
}

/** VDPpset6() ***********************************************/
/** Set a pixel on screen 6                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset6(int MXD, int DX, int DY, uint8_t CL, uint8_t OP)
{
	uint8_t SH = ((~DX)&3)<<1;

	VDPpsetlowlevel(VDP_VRMP6(MXD, DX, DY), CL << SH, ~(3<<SH), OP);
}

/** VDPpset7() ***********************************************/
/** Set a pixel on screen 7                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset7(int MXD, int DX, int DY, uint8_t CL, uint8_t OP)
{
	uint8_t SH = ((~DX)&1)<<2;

	VDPpsetlowlevel(VDP_VRMP7(MXD, DX, DY), CL << SH, ~(15<<SH), OP);
}

/** VDPpset8() ***********************************************/
/** Set a pixel on screen 8                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset8(int MXD, int DX, int DY, uint8_t CL, uint8_t OP)
{
	VDPpsetlowlevel(VDP_VRMP8(MXD, DX, DY), CL, 0, OP);
}

/** VDPpset() ************************************************/
/** Set a pixel on a screen                                 **/
/*************************************************************/
inline void v99x8_device::VDPpset(uint8_t SM, int MXD, int DX, int DY, uint8_t CL, uint8_t OP)
{
	switch (SM) {
	case 0: VDPpset5(MXD, DX, DY, CL, OP); break;
	case 1: VDPpset6(MXD, DX, DY, CL, OP); break;
	case 2: VDPpset7(MXD, DX, DY, CL, OP); break;
	case 3: VDPpset8(MXD, DX, DY, CL, OP); break;
	}
}

/** get_vdp_timing_value() **************************************/
/** Get timing value for a certain VDP command              **/
/*************************************************************/
int v99x8_device::get_vdp_timing_value(const int *timing_values)
{
	return(timing_values[((m_cont_reg[1]>>6)&1)|(m_cont_reg[8]&2)|((m_cont_reg[9]<<1)&4)]);
}

/** SrchEgine()** ********************************************/
/** Search a dot                                            **/
/*************************************************************/
void v99x8_device::srch_engine()
{
	int SX=m_mmc.SX;
	int SY=m_mmc.SY;
	int TX=m_mmc.TX;
	int ANX=m_mmc.ANX;
	uint8_t CL=m_mmc.CL;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(srch_timing);
	cnt = m_vdp_ops_count;

	#define post_srch(MX) \
	{ m_stat_reg[2]|=0x10; /* Border detected */ break; } \
	if ((SX+=TX) & MX) { m_stat_reg[2] &= 0xEF; /* Border not detected */ break; }
	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop if ((VDPpoint5(MXD, SX, SY)==CL) ^ANX)  post_srch(256) post_loop
			break;
	case V9938_MODE_GRAPHIC5: pre_loop if ((VDPpoint6(MXD, SX, SY)==CL) ^ANX)  post_srch(512) post_loop
			break;
	case V9938_MODE_GRAPHIC6: pre_loop if ((VDPpoint7(MXD, SX, SY)==CL) ^ANX)  post_srch(512) post_loop
			break;
	case V9938_MODE_GRAPHIC7: pre_loop if ((VDPpoint8(MXD, SX, SY)==CL) ^ANX)  post_srch(256) post_loop
			break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2] &= 0xFE;
		m_vdp_engine = nullptr;
		// Update SX in VDP registers
		m_stat_reg[8] = SX & 0xFF;
		m_stat_reg[9] = (SX>>8) | 0xFE;
	}
	else {
		m_mmc.SX=SX;
	}
}

/** LineEgine()** ********************************************/
/** Draw a line                                             **/
/*************************************************************/
void v99x8_device::line_engine()
{
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ASX=m_mmc.ASX;
	int ADX=m_mmc.ADX;
	uint8_t CL=m_mmc.CL;
	uint8_t LO=m_mmc.LO;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(line_timing);
	cnt = m_vdp_ops_count;

	#define post_linexmaj(MX) \
	DX+=TX; \
	if ((ASX-=NY)<0) { \
		ASX+=NX; \
		DY+=TY; \
	} \
	ASX&=1023; /* Mask to 10 bits range */\
	if (ADX++==NX || (DX&MX)) \
		break; \
	post_loop

	#define post_lineymaj(MX) \
	DY+=TY; \
	if ((ASX-=NY)<0) { \
		ASX+=NX; \
		DX+=TX; \
	} \
	ASX&=1023; /* Mask to 10 bits range */\
	if (ADX++==NX || (DX&MX)) \
		break; \
	post_loop

	if ((m_cont_reg[45]&0x01)==0)
		// X-Axis is major direction
	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop VDPpset5(MXD, DX, DY, CL, LO); post_linexmaj(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop VDPpset6(MXD, DX, DY, CL, LO); post_linexmaj(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop VDPpset7(MXD, DX, DY, CL, LO); post_linexmaj(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop VDPpset8(MXD, DX, DY, CL, LO); post_linexmaj(256)
		break;
	}
	else
		// Y-Axis is major direction
	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop VDPpset5(MXD, DX, DY, CL, LO); post_lineymaj(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop VDPpset6(MXD, DX, DY, CL, LO); post_lineymaj(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop VDPpset7(MXD, DX, DY, CL, LO); post_lineymaj(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop VDPpset8(MXD, DX, DY, CL, LO); post_lineymaj(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=nullptr;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.DX=DX;
		m_mmc.DY=DY;
		m_mmc.ASX=ASX;
		m_mmc.ADX=ADX;
	}
}

/** lmmv_engine() *********************************************/
/** VDP -> Vram                                             **/
/*************************************************************/
void v99x8_device::lmmv_engine()
{
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ADX=m_mmc.ADX;
	int ANX=m_mmc.ANX;
	uint8_t CL=m_mmc.CL;
	uint8_t LO=m_mmc.LO;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(lmmv_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop VDPpset5(MXD, ADX, DY, CL, LO); post__x_y(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop VDPpset6(MXD, ADX, DY, CL, LO); post__x_y(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop VDPpset7(MXD, ADX, DY, CL, LO); post__x_y(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop VDPpset8(MXD, ADX, DY, CL, LO); post__x_y(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=nullptr;
		if (!NY)
			DY+=TY;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
	}
	else {
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ANX=ANX;
		m_mmc.ADX=ADX;
	}
}

/** lmmm_engine() *********************************************/
/** Vram -> Vram                                            **/
/*************************************************************/
void v99x8_device::lmmm_engine()
{
	int SX=m_mmc.SX;
	int SY=m_mmc.SY;
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ASX=m_mmc.ASX;
	int ADX=m_mmc.ADX;
	int ANX=m_mmc.ANX;
	uint8_t LO=m_mmc.LO;
	int MXS = m_mmc.MXS;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(lmmm_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop VDPpset5(MXD, ADX, DY, VDPpoint5(MXS, ASX, SY), LO); post_xxyy(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop VDPpset6(MXD, ADX, DY, VDPpoint6(MXS, ASX, SY), LO); post_xxyy(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop VDPpset7(MXD, ADX, DY, VDPpoint7(MXS, ASX, SY), LO); post_xxyy(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop VDPpset8(MXD, ADX, DY, VDPpoint8(MXS, ASX, SY), LO); post_xxyy(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=nullptr;
		if (!NY) {
			SY+=TY;
			DY+=TY;
		}
		else
			if (SY==-1)
			DY+=TY;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
		m_cont_reg[34]=SY & 0xFF;
		m_cont_reg[35]=(SY>>8) & 0x03;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.SY=SY;
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ANX=ANX;
		m_mmc.ASX=ASX;
		m_mmc.ADX=ADX;
	}
}

/** lmcm_engine() *********************************************/
/** Vram -> CPU                                             **/
/*************************************************************/
void v99x8_device::lmcm_engine()
{
	if ((m_stat_reg[2]&0x80)!=0x80) {
		m_stat_reg[7]=m_cont_reg[44]=VDP_POINT(((m_mode >= 5) && (m_mode <= 8)) ? (m_mode-5) : 0, m_mmc.MXS, m_mmc.ASX, m_mmc.SY);
		m_vdp_ops_count-=get_vdp_timing_value(lmmv_timing);
		m_stat_reg[2]|=0x80;

		if (!--m_mmc.ANX || ((m_mmc.ASX+=m_mmc.TX)&m_mmc.MX)) {
			if (!(--m_mmc.NY & 1023) || (m_mmc.SY+=m_mmc.TY)==-1) {
				m_stat_reg[2]&=0xFE;
				m_vdp_engine=nullptr;
				if (!m_mmc.NY)
					m_mmc.DY+=m_mmc.TY;
				m_cont_reg[42]=m_mmc.NY & 0xFF;
				m_cont_reg[43]=(m_mmc.NY>>8) & 0x03;
				m_cont_reg[34]=m_mmc.SY & 0xFF;
				m_cont_reg[35]=(m_mmc.SY>>8) & 0x03;
			}
			else {
				m_mmc.ASX=m_mmc.SX;
				m_mmc.ANX=m_mmc.NX;
			}
		}
	}
}

/** lmmc_engine() *********************************************/
/** CPU -> Vram                                             **/
/*************************************************************/
void v99x8_device::lmmc_engine()
{
	if ((m_stat_reg[2]&0x80)!=0x80) {
		uint8_t SM=((m_mode >= 5) && (m_mode <= 8)) ? (m_mode-5) : 0;

		m_stat_reg[7]=m_cont_reg[44]&=Mask[SM];
		VDP_PSET(SM, m_mmc.MXD, m_mmc.ADX, m_mmc.DY, m_cont_reg[44], m_mmc.LO);
		m_vdp_ops_count-=get_vdp_timing_value(lmmv_timing);
		m_stat_reg[2]|=0x80;

		if (!--m_mmc.ANX || ((m_mmc.ADX+=m_mmc.TX)&m_mmc.MX)) {
			if (!(--m_mmc.NY&1023) || (m_mmc.DY+=m_mmc.TY)==-1) {
				m_stat_reg[2]&=0xFE;
				m_vdp_engine=nullptr;
				if (!m_mmc.NY)
					m_mmc.DY+=m_mmc.TY;
				m_cont_reg[42]=m_mmc.NY & 0xFF;
				m_cont_reg[43]=(m_mmc.NY>>8) & 0x03;
				m_cont_reg[38]=m_mmc.DY & 0xFF;
				m_cont_reg[39]=(m_mmc.DY>>8) & 0x03;
			}
			else {
				m_mmc.ADX=m_mmc.DX;
				m_mmc.ANX=m_mmc.NX;
			}
		}
	}
}

/** hmmv_engine() *********************************************/
/** VDP --> Vram                                            **/
/*************************************************************/
void v99x8_device::hmmv_engine()
{
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ADX=m_mmc.ADX;
	int ANX=m_mmc.ANX;
	uint8_t CL=m_mmc.CL;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(hmmv_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop vram_barrier_w(VDP_VRMP5(MXD, ADX, DY), CL); post__x_y(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop vram_barrier_w(VDP_VRMP6(MXD, ADX, DY), CL); post__x_y(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop vram_barrier_w(VDP_VRMP7(MXD, ADX, DY), CL); post__x_y(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop vram_barrier_w(VDP_VRMP8(MXD, ADX, DY), CL); post__x_y(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=nullptr;
		if (!NY)
			DY+=TY;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ANX=ANX;
		m_mmc.ADX=ADX;
	}
}

/** hmmm_engine() *********************************************/
/** Vram -> Vram                                            **/
/*************************************************************/
void v99x8_device::hmmm_engine()
{
	int SX=m_mmc.SX;
	int SY=m_mmc.SY;
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NX=m_mmc.NX;
	int NY=m_mmc.NY;
	int ASX=m_mmc.ASX;
	int ADX=m_mmc.ADX;
	int ANX=m_mmc.ANX;
	int MXS = m_mmc.MXS;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(hmmm_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop vram_barrier_w(VDP_VRMP5(MXD, ADX, DY), vram_barrier(VDP_VRMP5(MXS, ASX, SY))); post_xxyy(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop vram_barrier_w(VDP_VRMP6(MXD, ADX, DY), vram_barrier(VDP_VRMP6(MXS, ASX, SY))); post_xxyy(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop vram_barrier_w(VDP_VRMP7(MXD, ADX, DY), vram_barrier(VDP_VRMP7(MXS, ASX, SY))); post_xxyy(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop vram_barrier_w(VDP_VRMP8(MXD, ADX, DY), vram_barrier(VDP_VRMP8(MXS, ASX, SY))); post_xxyy(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=nullptr;
		if (!NY) {
			SY+=TY;
			DY+=TY;
		}
		else
			if (SY==-1)
			DY+=TY;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
		m_cont_reg[34]=SY & 0xFF;
		m_cont_reg[35]=(SY>>8) & 0x03;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.SY=SY;
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ANX=ANX;
		m_mmc.ASX=ASX;
		m_mmc.ADX=ADX;
	}
}

/** ymmm_engine() *********************************************/
/** Vram -> Vram                                            **/
/*************************************************************/

void v99x8_device::ymmm_engine()
{
	int SY=m_mmc.SY;
	int DX=m_mmc.DX;
	int DY=m_mmc.DY;
	int TX=m_mmc.TX;
	int TY=m_mmc.TY;
	int NY=m_mmc.NY;
	int ADX=m_mmc.ADX;
	int MXD = m_mmc.MXD;
	int cnt;
	int delta;

	delta = get_vdp_timing_value(ymmm_timing);
	cnt = m_vdp_ops_count;

	switch (m_mode) {
	default:
	case V9938_MODE_GRAPHIC4: pre_loop vram_barrier_w(VDP_VRMP5(MXD, ADX, DY), vram_barrier(VDP_VRMP5(MXD, ADX, SY))); post__xyy(256)
		break;
	case V9938_MODE_GRAPHIC5: pre_loop vram_barrier_w(VDP_VRMP6(MXD, ADX, DY), vram_barrier(VDP_VRMP6(MXD, ADX, SY))); post__xyy(512)
		break;
	case V9938_MODE_GRAPHIC6: pre_loop vram_barrier_w(VDP_VRMP7(MXD, ADX, DY), vram_barrier(VDP_VRMP7(MXD, ADX, SY))); post__xyy(512)
		break;
	case V9938_MODE_GRAPHIC7: pre_loop vram_barrier_w(VDP_VRMP8(MXD, ADX, DY), vram_barrier(VDP_VRMP8(MXD, ADX, SY))); post__xyy(256)
		break;
	}

	if ((m_vdp_ops_count=cnt)>0) {
		// Command execution done
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=nullptr;
		if (!NY) {
			SY+=TY;
			DY+=TY;
		}
		else
			if (SY==-1)
			DY+=TY;
		m_cont_reg[42]=NY & 0xFF;
		m_cont_reg[43]=(NY>>8) & 0x03;
		m_cont_reg[34]=SY & 0xFF;
		m_cont_reg[35]=(SY>>8) & 0x03;
		m_cont_reg[38]=DY & 0xFF;
		m_cont_reg[39]=(DY>>8) & 0x03;
	}
	else {
		m_mmc.SY=SY;
		m_mmc.DY=DY;
		m_mmc.NY=NY;
		m_mmc.ADX=ADX;
	}
}

/** hmmc_engine() *********************************************/
/** CPU -> Vram                                             **/
/*************************************************************/
void v99x8_device::hmmc_engine()
{
	if ((m_stat_reg[2]&0x80)!=0x80) {
		vram_barrier_w(VDP_VRMP(((m_mode >= 5) && (m_mode <= 8)) ? (m_mode-5) : 0, m_mmc.MXD, m_mmc.ADX, m_mmc.DY), m_cont_reg[44]);
		m_vdp_ops_count -= get_vdp_timing_value(hmmv_timing);
		m_stat_reg[2]|=0x80;

		if (!--m_mmc.ANX || ((m_mmc.ADX+=m_mmc.TX)&m_mmc.MX)) {
			if (!(--m_mmc.NY&1023) || (m_mmc.DY+=m_mmc.TY)==-1) {
				m_stat_reg[2]&=0xFE;
				m_vdp_engine=nullptr;
				if (!m_mmc.NY)
					m_mmc.DY+=m_mmc.TY;
				m_cont_reg[42]=m_mmc.NY & 0xFF;
				m_cont_reg[43]=(m_mmc.NY>>8) & 0x03;
				m_cont_reg[38]=m_mmc.DY & 0xFF;
				m_cont_reg[39]=(m_mmc.DY>>8) & 0x03;
			}
			else {
				m_mmc.ADX=m_mmc.DX;
				m_mmc.ANX=m_mmc.NX;
			}
		}
	}
}

/** VDPWrite() ***********************************************/
/** Use this function to transfer pixel(s) from CPU to m_ **/
/*************************************************************/
void v99x8_device::cpu_to_vdp(uint8_t V)
{
	m_stat_reg[2]&=0x7F;
	m_stat_reg[7]=m_cont_reg[44]=V;
	if(m_vdp_engine&&(m_vdp_ops_count>0)) (this->*m_vdp_engine)();
}

/** VDPRead() ************************************************/
/** Use this function to transfer pixel(s) from VDP to CPU. **/
/*************************************************************/
uint8_t v99x8_device::vdp_to_cpu()
{
	m_stat_reg[2]&=0x7F;
	if(m_vdp_engine&&(m_vdp_ops_count>0)) (this->*m_vdp_engine)();
	return(m_cont_reg[44]);
}

/** report_vdp_command() ***************************************/
/** Report VDP Command to be executed                       **/
/*************************************************************/
void v99x8_device::report_vdp_command(uint8_t Op) { (void)Op; }

/** VDPDraw() ************************************************/
/** Perform a given V9938 operation Op.                     **/
/*************************************************************/
uint8_t v99x8_device::command_unit_w(uint8_t Op)
{
	// V9938 ops only work in SCREENs 5-8
	if (m_mode<5 || m_mode>8)
		return(0);

	int SM = m_mode-5;         // Screen mode index 0..3

	// PORT: rejected opcodes must not replace the saved active engine identity.
	if ((Op >> 4) > 0 && (Op >> 4) < 4) return 0;
	m_mmc.CM = Op>>4;
	if ((m_mmc.CM & 0x0C) != 0x0C && m_mmc.CM != 0)
		// Dot operation: use only relevant bits of color
	m_stat_reg[7]=(m_cont_reg[44]&=Mask[SM]);

	//  if(Verbose&0x02)
	report_vdp_command(Op);

	if ((m_vdp_engine != nullptr) && (m_mmc.CM != CM_ABRT))
		LOGMASKED(LOG_WARN, "Command overrun; previous command not completed\n");

	switch(Op>>4) {
	case CM_ABRT:
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=nullptr;
		return 1;
	case CM_POINT:
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=nullptr;
		m_stat_reg[7]=m_cont_reg[44]=
		VDP_POINT(SM, (m_cont_reg[45] & 0x10) != 0,
			m_cont_reg[32]+((int)m_cont_reg[33]<<8),
			m_cont_reg[34]+((int)m_cont_reg[35]<<8));
		return 1;
	case CM_PSET:
		m_stat_reg[2]&=0xFE;
		m_vdp_engine=nullptr;
		VDP_PSET(SM, (m_cont_reg[45] & 0x20) != 0,
			m_cont_reg[36]+((int)m_cont_reg[37]<<8),
			m_cont_reg[38]+((int)m_cont_reg[39]<<8),
			m_cont_reg[44],
			Op&0x0F);
		return 1;
	case CM_SRCH:
		m_vdp_engine=&v99x8_device::srch_engine;
		break;
	case CM_LINE:
		m_vdp_engine=&v99x8_device::line_engine;
		break;
	case CM_LMMV:
		m_vdp_engine=&v99x8_device::lmmv_engine;
		break;
	case CM_LMMM:
		m_vdp_engine=&v99x8_device::lmmm_engine;
		break;
	case CM_LMCM:
		m_vdp_engine=&v99x8_device::lmcm_engine;
		break;
	case CM_LMMC:
		m_vdp_engine=&v99x8_device::lmmc_engine;
		break;
	case CM_HMMV:
		m_vdp_engine=&v99x8_device::hmmv_engine;
		break;
	case CM_HMMM:
		m_vdp_engine=&v99x8_device::hmmm_engine;
		break;
	case CM_YMMM:
		m_vdp_engine=&v99x8_device::ymmm_engine;
		break;
	case CM_HMMC:
		m_vdp_engine=&v99x8_device::hmmc_engine;
		break;
	default:
		LOGMASKED(LOG_WARN, "Unrecognized opcode %02Xh\n",Op);
		return(0);
	}

	// Fetch unconditional arguments
	m_mmc.SX = (m_cont_reg[32]+((int)m_cont_reg[33]<<8)) & 511;
	m_mmc.SY = (m_cont_reg[34]+((int)m_cont_reg[35]<<8)) & 1023;
	m_mmc.DX = (m_cont_reg[36]+((int)m_cont_reg[37]<<8)) & 511;
	m_mmc.DY = (m_cont_reg[38]+((int)m_cont_reg[39]<<8)) & 1023;
	m_mmc.NY = (m_cont_reg[42]+((int)m_cont_reg[43]<<8)) & 1023;
	m_mmc.TY = m_cont_reg[45]&0x08? -1:1;
	m_mmc.MX = PPL[SM];
	m_mmc.CL = m_cont_reg[44];
	m_mmc.LO = Op&0x0F;
	m_mmc.MXS = (m_cont_reg[45] & 0x10) != 0;
	m_mmc.MXD = (m_cont_reg[45] & 0x20) != 0;

	// Argument depends on uint8_t or dot operation
	if ((m_mmc.CM & 0x0C) == 0x0C) {
		m_mmc.TX = m_cont_reg[45]&0x04? -PPB[SM]:PPB[SM];
		m_mmc.NX = ((m_cont_reg[40]+((int)m_cont_reg[41]<<8)) & 1023)/PPB[SM];
	}
	else {
		m_mmc.TX = m_cont_reg[45]&0x04? -1:1;
		m_mmc.NX = (m_cont_reg[40]+((int)m_cont_reg[41]<<8)) & 1023;
	}

	// X loop variables are treated specially for LINE command
	if (m_mmc.CM == CM_LINE) {
		m_mmc.ASX=((m_mmc.NX-1)>>1);
		m_mmc.ADX=0;
	}
	else {
		m_mmc.ASX = m_mmc.SX;
		m_mmc.ADX = m_mmc.DX;
	}

	// NX loop variable is treated specially for SRCH command
	if (m_mmc.CM == CM_SRCH)
		m_mmc.ANX=(m_cont_reg[45]&0x02)!=0; // Do we look for "==" or "!="?
	else
		m_mmc.ANX = m_mmc.NX;

	// Command execution started
	m_stat_reg[2]|=0x01;

	// Start execution if we still have time slices
	if(m_vdp_engine&&(m_vdp_ops_count>0)) (this->*m_vdp_engine)();

	// Operation successfully initiated
	return(1);
}

/** LoopVDP() ************************************************
Run X steps of active VDP command
*************************************************************/
void v99x8_device::update_command()
{
	if(m_vdp_ops_count<=0)
	{
		m_vdp_ops_count+=13662;
		if(m_vdp_engine&&(m_vdp_ops_count>0)) (this->*m_vdp_engine)();
	}
	else
	{
		m_vdp_ops_count=13662;
		if(m_vdp_engine) (this->*m_vdp_engine)();
	}
}

void v99x8_device::device_post_load() // TODO: is there a better way to restore this?
{
	m_vdp_engine = nullptr;
	if (!(m_stat_reg[2] & 1)) return;
	switch(m_mmc.CM)
	{
	case CM_ABRT:
	case CM_POINT:
	case CM_PSET:
		m_vdp_engine=nullptr;
		break;
	case CM_SRCH:
		m_vdp_engine=&v99x8_device::srch_engine;
		break;
	case CM_LINE:
		m_vdp_engine=&v99x8_device::line_engine;
		break;
	case CM_LMMV:
		m_vdp_engine=&v99x8_device::lmmv_engine;
		break;
	case CM_LMMM:
		m_vdp_engine=&v99x8_device::lmmm_engine;
		break;
	case CM_LMCM:
		m_vdp_engine=&v99x8_device::lmcm_engine;
		break;
	case CM_LMMC:
		m_vdp_engine=&v99x8_device::lmmc_engine;
		break;
	case CM_HMMV:
		m_vdp_engine=&v99x8_device::hmmv_engine;
		break;
	case CM_HMMM:
		m_vdp_engine=&v99x8_device::hmmm_engine;
		break;
	case CM_YMMM:
		m_vdp_engine=&v99x8_device::ymmm_engine;
		break;
	case CM_HMMC:
		m_vdp_engine=&v99x8_device::hmmc_engine;
		break;
	}
}

// PORT: this file is included by the card; keep command macros local.
#undef VDP_VRMP5
#undef VDP_VRMP6
#undef VDP_VRMP7
#undef VDP_VRMP8
#undef VDP_VRMP
#undef VDP_POINT
#undef VDP_PSET
#undef CM_ABRT
#undef CM_POINT
#undef CM_PSET
#undef CM_SRCH
#undef CM_LINE
#undef CM_LMMV
#undef CM_LMMM
#undef CM_LMCM
#undef CM_LMMC
#undef CM_HMMV
#undef CM_HMMM
#undef CM_YMMM
#undef CM_HMMC
#undef pre_loop
#undef post_loop
#undef post__x_y
#undef post__xyy
#undef post_xxyy
#undef post_srch
#undef post_linexmaj
#undef post_lineymaj

// =====================================================================
//  State
// =====================================================================

// PORT: MAME's 44 save_item() calls registered every field below with the
// save system.  The port writes them field-wise into a caller buffer, which
// also covers VRAM -- MAME's address space carried that itself.
uint64_t v99x8_device::state_size() const
{
	return STATE_HEADER_SIZE + VRAM_TOTAL_SIZE + COMMAND_STATE_SIZE;
}

void v99x8_device::save_state(uint8_t *buffer) const
{
	if (!buffer)
		return;
	std::memcpy(buffer, m_pal_reg, sizeof(m_pal_reg));
	buffer += sizeof(m_pal_reg);
	std::memcpy(buffer, m_stat_reg, sizeof(m_stat_reg));
	buffer += sizeof(m_stat_reg);
	std::memcpy(buffer, m_cont_reg, sizeof(m_cont_reg));
	buffer += sizeof(m_cont_reg);
	const int32_t scalars[] = {
		m_offset_x, m_offset_y, m_visible_y, m_mode,
		m_pal_write_first, m_cmd_write_first,
		m_scanline, m_blink, m_blink_count, m_vram_size,
		m_scanline_start, m_vblank_start, m_scanline_max, m_height,
		m_pal_ntsc, m_v9958_sp_mode, m_second_field
	};
	for (size_t i = 0; i < std::size(scalars); ++i)
		srz80::sdk::state::put(buffer + 4 * i, scalars[i]);
	buffer += sizeof(scalars);
	const int16_t mouse[] = { m_mx_delta, m_my_delta };
	for (size_t i = 0; i < std::size(mouse); ++i)
		srz80::sdk::state::put(buffer + 2 * i, mouse[i]);
	buffer += sizeof(mouse);
	std::memcpy(buffer, &m_button_state, sizeof(m_button_state));
	buffer += sizeof(m_button_state);
	std::memcpy(buffer, &m_pal_write, sizeof(m_pal_write));
	buffer += sizeof(m_pal_write);
	std::memcpy(buffer, &m_cmd_write, sizeof(m_cmd_write));
	buffer += sizeof(m_cmd_write);
	std::memcpy(buffer, &m_read_ahead, sizeof(m_read_ahead));
	buffer += sizeof(m_read_ahead);
	srz80::sdk::state::put(buffer, m_address_latch);
	buffer += sizeof(m_address_latch);
	std::memcpy(buffer, &m_int_state, sizeof(m_int_state));
	buffer += sizeof(m_int_state);
	std::memset(buffer, 0, VRAM_TOTAL_SIZE);
	std::memcpy(buffer, m_vram.data(), size_t(m_vram_size));
	buffer += VRAM_TOTAL_SIZE;
	const int32_t command[] = { m_mmc.SX, m_mmc.SY, m_mmc.DX, m_mmc.DY, m_mmc.TX, m_mmc.TY, m_mmc.NX, m_mmc.NY, m_mmc.MX, m_mmc.ASX, m_mmc.ADX, m_mmc.ANX, m_vdp_ops_count };
	for (size_t i = 0; i < std::size(command); ++i)
		srz80::sdk::state::put(buffer + 4 * i, command[i]);
	buffer += sizeof(command);
	*buffer++ = m_mmc.CL;
	*buffer++ = m_mmc.LO;
	*buffer++ = m_mmc.CM;
	*buffer++ = m_mmc.MXS;
	*buffer++ = m_mmc.MXD;

}

bool v99x8_device::load_state(const uint8_t *buffer, uint64_t size)
{
	if (!buffer || size != state_size())
		return false;
	int32_t validated[17];
	for (size_t i = 0; i < std::size(validated); ++i)
		validated[i] = srz80::sdk::state::get<int32_t>(buffer + 90 + 4 * i);
	if (validated[0] < 0 || validated[0] > 16 || validated[1] < -7 || validated[1] > 8 ||
	    (validated[2] != 192 && validated[2] != 212) || validated[3] < 0 || validated[3] > V9938_MODE_UNKNOWN ||
	    validated[9] != m_vram_size || (validated[13] != 262 && validated[13] != 313) ||
	    validated[6] < 0 || validated[6] >= validated[13]) return false;
	std::memcpy(m_pal_reg, buffer, sizeof(m_pal_reg));
	buffer += sizeof(m_pal_reg);
	std::memcpy(m_stat_reg, buffer, sizeof(m_stat_reg));
	buffer += sizeof(m_stat_reg);
	std::memcpy(m_cont_reg, buffer, sizeof(m_cont_reg));
	buffer += sizeof(m_cont_reg);
	int32_t scalars[17];
	for (size_t i = 0; i < std::size(scalars); ++i)
		scalars[i] = srz80::sdk::state::get<int32_t>(buffer + 4 * i);
	buffer += sizeof(scalars);
	m_offset_x = scalars[0];
	m_offset_y = scalars[1];
	m_visible_y = scalars[2];
	m_mode = scalars[3];
	m_pal_write_first = scalars[4];
	m_cmd_write_first = scalars[5];
	m_scanline = scalars[6];
	m_blink = scalars[7];
	m_blink_count = scalars[8];
	m_vram_size = scalars[9];
	m_scanline_start = scalars[10];
	m_vblank_start = scalars[11];
	m_scanline_max = scalars[12];
	m_height = scalars[13];
	m_pal_ntsc = uint8_t(scalars[14]);
	m_v9958_sp_mode = uint8_t(scalars[15]);
	m_second_field = uint8_t(scalars[16]);
	int16_t mouse[2];
	for (size_t i = 0; i < std::size(mouse); ++i)
		mouse[i] = srz80::sdk::state::get<int16_t>(buffer + 2 * i);
	buffer += sizeof(mouse);
	m_mx_delta = mouse[0];
	m_my_delta = mouse[1];
	std::memcpy(&m_button_state, buffer, sizeof(m_button_state));
	buffer += sizeof(m_button_state);
	std::memcpy(&m_pal_write, buffer, sizeof(m_pal_write));
	buffer += sizeof(m_pal_write);
	std::memcpy(&m_cmd_write, buffer, sizeof(m_cmd_write));
	buffer += sizeof(m_cmd_write);
	std::memcpy(&m_read_ahead, buffer, sizeof(m_read_ahead));
	buffer += sizeof(m_read_ahead);
	m_address_latch = srz80::sdk::state::get<uint16_t>(buffer);
	buffer += sizeof(m_address_latch);
	std::memcpy(&m_int_state, buffer, sizeof(m_int_state));
	buffer += sizeof(m_int_state);
	std::fill(m_vram.begin(), m_vram.end(), uint8_t{0});
	std::memcpy(m_vram.data(), buffer, size_t(m_vram_size));
	m_mmc = {};
	m_vdp_ops_count = 0;
	{
		buffer += VRAM_TOTAL_SIZE;
		int32_t command[13];
		for (size_t i = 0; i < std::size(command); ++i)
			command[i] = srz80::sdk::state::get<int32_t>(buffer + 4 * i);
		buffer += sizeof(command);
		m_mmc.SX = command[0];
		m_mmc.SY = command[1];
		m_mmc.DX = command[2];
		m_mmc.DY = command[3];
		m_mmc.TX = command[4];
		m_mmc.TY = command[5];
		m_mmc.NX = command[6];
		m_mmc.NY = command[7];
		m_mmc.MX = command[8];
		m_mmc.ASX = command[9];
		m_mmc.ADX = command[10];
		m_mmc.ANX = command[11];
		m_vdp_ops_count = command[12];
		m_mmc.CL = *buffer++;
		m_mmc.LO = *buffer++;
		m_mmc.CM = *buffer++;
		m_mmc.MXS = *buffer++;
		m_mmc.MXD = *buffer++;

	}
	device_post_load();

	// PORT: the palette tables are derived state.  MAME's palette interface
	// was covered by save_item() on m_pal_reg; rebuilt here from m_pal_reg so
	// a load cannot leave the two out of step.
	rebuild_palette_registers();
	return true;
}

// PORT: set_pen16/set_pen256 are paths into the palette tables, so a state
// load rebuilds them the same way register 16 does.
void v99x8_device::rebuild_palette_registers()
{
	for (int i = 0; i < 16; ++i)
	{
		const uint8_t high = m_pal_reg[i*2];
		const uint8_t low = m_pal_reg[i*2+1];
		set_pen16(i, rgb3((high >> 4) & 7, low & 7, high & 7));
	}

	for (int i = 0; i < 256; ++i)
	{
		int red = (i << 1) & 6; if (red == 6) red++;
		set_pen256(i, rgb3((i >> 2) & 7, (i >> 5) & 7, uint8_t(red)));
	}
}

// ---------------------------------------------------------------------
//  Mode renderers, verbatim from MAME 0.289 v9938.cpp
//
//  Extracted from v9938.cpp lines 918-1259.  Only two mechanical
//  rewrites were applied and both are listed in the header of this file:
//    m_vram_space->read_byte(X)  ->  vram_barrier(X)
//    m_vram_space->write_byte(X, V) -> vram_barrier_w(X, V)
//  Everything else, including the sprite blitters below, is untouched.
// ---------------------------------------------------------------------


void v99x8_device::default_border(uint32_t *ln)
{
	pen_t pen;
	int i;

	pen = pen16(m_cont_reg[7] & 0x0f);
	i = V9938_LONG_WIDTH;
	while (i--) *ln++ = pen;
}

void v99x8_device::graphic7_border(uint32_t *ln)
{
	pen_t pen;
	int i;

	pen = pen256(m_cont_reg[7]);
	i = V9938_LONG_WIDTH;
	while (i--) *ln++ = pen;
}

void v99x8_device::graphic5_border(uint32_t *ln)
{
	int i;
	pen_t pen0;
	pen_t pen1;

	pen1 = pen16(m_cont_reg[7] & 0x03);
	pen0 = pen16((m_cont_reg[7] >> 2) & 0x03);
	i = V9938_LONG_WIDTH / 2;
	while (i--) { *ln++ = pen0; *ln++ = pen1; }
}

void v99x8_device::mode_text1(uint32_t *ln, int line)
{
	int pattern, x, xx, name, xxx;
	pen_t fg, bg, pen;
	int nametbl_addr, patterntbl_addr;

	patterntbl_addr = m_cont_reg[4] << 11;
	nametbl_addr = m_cont_reg[2] << 10;

	fg = pen16(m_cont_reg[7] >> 4);
	bg = pen16(m_cont_reg[7] & 15);

	name = (line/8)*40;

	pen = pen16(m_cont_reg[7] & 0x0f);

	xxx = (m_offset_x + 8) * 2;
	while (xxx--) *ln++ = pen;

	for (x=0;x<40;x++)
	{
		pattern = vram_barrier(patterntbl_addr + (vram_barrier(nametbl_addr + name) * 8) +
			((line + m_cont_reg[23]) & 7));
		for (xx=0;xx<6;xx++)
		{
			*ln++ = (pattern & 0x80) ? fg : bg;
			*ln++ = (pattern & 0x80) ? fg : bg;
			pattern <<= 1;
		}
		/* width height 212, characters start repeating at the bottom */
		name = (name + 1) & 0x3ff;
	}

	xxx = ((16 - m_offset_x) + 8) * 2;
	while (xxx--) *ln++ = pen;
}

void v99x8_device::mode_text2(uint32_t *ln, int line)
{
	int pattern, x, charcode, name, xxx, patternmask, colourmask;
	pen_t fg, bg, fg0, bg0, pen;
	int nametbl_addr, patterntbl_addr, colourtbl_addr;

	patterntbl_addr = m_cont_reg[4] << 11;
	colourtbl_addr =  ((m_cont_reg[3] & 0xf8) << 6) + (m_cont_reg[10] << 14);
	#if 0
	colourmask = ((m_cont_reg[3] & 7) << 5) | 0x1f; /* cause a bug in Forth+ v1.0 on Geneve */
	#else
	colourmask = ((m_cont_reg[3] & 7) << 6) | 0x3f; /* verify! */
	#endif
	nametbl_addr = ((m_cont_reg[2] & 0xfc) << 10);
	patternmask = ((m_cont_reg[2] & 3) << 10) | 0x3ff; /* seems correct */

	fg = pen16(m_cont_reg[7] >> 4);
	bg = pen16(m_cont_reg[7] & 15);
	fg0 = pen16(m_cont_reg[12] >> 4);
	bg0 = pen16(m_cont_reg[12] & 15);

	name = (line/8)*80;

	xxx = (m_offset_x + 8) * 2;
	pen = pen16(m_cont_reg[7] & 0x0f);
	while (xxx--) *ln++ = pen;

	for (x=0;x<80;x++)
	{
		charcode = vram_barrier(nametbl_addr + (name&patternmask));
		if (m_blink)
		{
			pattern = vram_barrier(colourtbl_addr + ((name/8)&colourmask));
			if (pattern & (0x80 >> (name & 7) ) )
			{
				pattern = vram_barrier(patterntbl_addr + ((charcode * 8) +
					((line + m_cont_reg[23]) & 7)));

				*ln++ = (pattern & 0x80) ? fg0 : bg0;
				*ln++ = (pattern & 0x40) ? fg0 : bg0;
				*ln++ = (pattern & 0x20) ? fg0 : bg0;
				*ln++ = (pattern & 0x10) ? fg0 : bg0;
				*ln++ = (pattern & 0x08) ? fg0 : bg0;
				*ln++ = (pattern & 0x04) ? fg0 : bg0;

				name++;
				continue;
			}
		}

		pattern = vram_barrier(patterntbl_addr + ((charcode * 8) +
			((line + m_cont_reg[23]) & 7)));

		*ln++ = (pattern & 0x80) ? fg : bg;
		*ln++ = (pattern & 0x40) ? fg : bg;
		*ln++ = (pattern & 0x20) ? fg : bg;
		*ln++ = (pattern & 0x10) ? fg : bg;
		*ln++ = (pattern & 0x08) ? fg : bg;
		*ln++ = (pattern & 0x04) ? fg : bg;

		name++;
	}

	xxx = (16 - m_offset_x + 8) * 2;
	while (xxx--) *ln++ = pen;
}

void v99x8_device::mode_multi(uint32_t *ln, int line)
{
	int nametbl_addr, patterntbl_addr, colour;
	int name, line2, x, xx;
	pen_t pen, pen_bg;

	nametbl_addr = (m_cont_reg[2] << 10);
	patterntbl_addr = (m_cont_reg[4] << 11);

	line2 = (line - m_cont_reg[23]) & 255;
	name = (line2/8)*32;

	pen_bg = pen16(m_cont_reg[7] & 0x0f);
	xx = m_offset_x * 2;
	while (xx--) *ln++ = pen_bg;

	for (x=0;x<32;x++)
	{
		colour = vram_barrier(patterntbl_addr + (vram_barrier(nametbl_addr + name) * 8) + ((line2/4)&7));
		pen = pen16(colour >> 4);
		/* eight pixels */
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		pen = pen16(colour & 15);
		/* eight pixels */
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		*ln++ = pen;
		name++;
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) *ln++ = pen_bg;
}

void v99x8_device::mode_graphic1(uint32_t *ln, int line)
{
	pen_t fg, bg, pen;
	int nametbl_addr, patterntbl_addr, colourtbl_addr;
	int pattern, x, xx, line2, name, charcode, colour, xxx;

	nametbl_addr = (m_cont_reg[2] << 10);
	colourtbl_addr = (m_cont_reg[3] << 6) + (m_cont_reg[10] << 14);
	patterntbl_addr = (m_cont_reg[4] << 11);

	line2 = (line - m_cont_reg[23]) & 255;

	name = (line2/8)*32;

	pen = pen16(m_cont_reg[7] & 0x0f);
	xxx = m_offset_x * 2;
	while (xxx--) *ln++ = pen;

	for (x=0;x<32;x++)
	{
		charcode = vram_barrier(nametbl_addr + name);
		colour = vram_barrier(colourtbl_addr + charcode/8);
		fg = pen16(colour >> 4);
		bg = pen16(colour & 15);
		pattern = vram_barrier(patterntbl_addr + (charcode * 8 + (line2 & 7)));

		for (xx=0;xx<8;xx++)
		{
			*ln++ = (pattern & 0x80) ? fg : bg;
			*ln++ = (pattern & 0x80) ? fg : bg;
			pattern <<= 1;
		}
		name++;
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) *ln++ = pen;
}

void v99x8_device::mode_graphic23(uint32_t *ln, int line)
{
	const int colourmask = ((m_cont_reg[3] & 0x7f) << 3) | 7;
	const int patternmask = ((m_cont_reg[4] & 0x03) << 8) | 0xff;
	const int scrolled_y = (line + m_cont_reg[23]) & 0xff;
	const int colourtbl_addr = ((m_cont_reg[3] & 0x80) << 6) + (m_cont_reg[10] << 14);
	const int patterntbl_addr = ((m_cont_reg[4] & 0x3c) << 11);
	const pen_t border_pen = pen16(m_cont_reg[7] & 0x0f);

	int nametbl_base = (m_cont_reg[2] << 10) + ((scrolled_y / 8) * 32);
	int nametbl_offset = (m_cont_reg[26] & 0x1f);

	if (BIT2(m_cont_reg[25], 0) && BIT2(m_cont_reg[26], 5))
		nametbl_base ^= 0x8000;

	for (int x = m_offset_x * 2; x > 0; x--)
		*ln++ = border_pen;

	int dot_scroll = m_cont_reg[27] & 0x07;
	int pixels_to_mask = BIT2(m_cont_reg[25], 1) ? 8 - dot_scroll : 0;
	int pixels_to_draw = 256 - dot_scroll;
	while (dot_scroll--)
	{
		*ln++ = border_pen;
		*ln++ = border_pen;
	}

	do
	{
		const int charcode = vram_barrier(nametbl_base + nametbl_offset) + ((scrolled_y & 0xc0) << 2);
		const u8 colour = vram_barrier(colourtbl_addr + ((charcode & colourmask) << 3) + (scrolled_y & 7));
		u8 pattern = vram_barrier(patterntbl_addr + ((charcode & patternmask) << 3) + (scrolled_y & 7));
		const pen_t fg = pen16(colour >> 4);
		const pen_t bg = pen16(colour & 0x0f);
		for (int x = 0; x < 8 && pixels_to_draw > 0; x++)
		{
			if (!pixels_to_mask)
			{
				*ln++ = (pattern & 0x80) ? fg : bg;
				*ln++ = (pattern & 0x80) ? fg : bg;
			}
			else
			{
				pixels_to_mask--;
				*ln++ = border_pen;
				*ln++ = border_pen;
			}
			pixels_to_draw--;
			pattern <<= 1;
		}
		nametbl_offset = (nametbl_offset + 1) & 0x1f;
		if (BIT2(m_cont_reg[25], 0) && !nametbl_offset)
			nametbl_base ^= 0x8000;
	}
	while (pixels_to_draw > 0);

	for (int x = (16 - m_offset_x) * 2; x > 0; x--)
		*ln++ = border_pen;
}

void v99x8_device::mode_graphic4(uint32_t *ln, int line)
{
	int linemask = ((m_cont_reg[2] & 0x1f) << 3) | 7;
	const int scrolled_y = ((line + m_cont_reg[23]) & linemask) & 0xff;
	const pen_t border_pen = pen16(m_cont_reg[7] & 0x0f);

	int nametbl_base = ((m_cont_reg[2] & 0x40) << 10) + (scrolled_y << 7);
	int nametbl_offset = (m_cont_reg[26] & 0x1f) << 2;
	if (!BIT2(m_cont_reg[25], 0) && BIT2(m_cont_reg[2], 5) && v9938_second_field())
		nametbl_base += 0x8000;
	if (BIT2(m_cont_reg[25], 0) && BIT2(m_cont_reg[26], 5))
		nametbl_base ^= 0x8000;

	for (int x = m_offset_x * 2; x > 0; x--)
		*ln++ = border_pen;

	int dot_scroll = m_cont_reg[27] & 0x07;
	int pixels_to_mask = BIT2(m_cont_reg[25], 1) ? 8 - dot_scroll : 0;
	int pixels_to_draw = 256 - dot_scroll;
	while (dot_scroll--)
	{
		*ln++ = border_pen;
		*ln++ = border_pen;
	}

	do
	{
		bool mask_pixel_1 = false;
		bool mask_pixel_2 = false;
		if (pixels_to_mask)
		{
			mask_pixel_1 = true;
			pixels_to_mask--;
			if (pixels_to_mask)
			{
				mask_pixel_2 = true;
				pixels_to_mask--;
			}
		}
		const u8 colour = vram_barrier(nametbl_base + nametbl_offset);
		const pen_t pen1 = mask_pixel_1 ? border_pen : pen16(colour >> 4);
		const pen_t pen2 = mask_pixel_2 ? border_pen : pen16(colour & 0x0f);
		*ln++ = pen1;
		*ln++ = pen1;
		pixels_to_draw--;
		if (pixels_to_draw)
		{
			*ln++ = pen2;
			*ln++ = pen2;
			pixels_to_draw--;
		}
		nametbl_offset = (nametbl_offset + 1) & 0x7f;
		if (BIT2(m_cont_reg[25], 0) && !nametbl_offset)
			nametbl_base ^= 0x8000;
	}
	while (pixels_to_draw > 0);

	for (int x = (16 - m_offset_x) * 2; x > 0; x--)
		*ln++ = border_pen;
}


// ---------------------------------------------------------------------
//  Sprite scan, verbatim from MAME 0.289 v9938.cpp lines 1551-1802
// ---------------------------------------------------------------------



// ---------------------------------------------------------------------
//  Remaining renderers (mode_unknown and the sprite blitters), verbatim
//  from MAME 0.289 v9938.cpp lines 1473-1551.
// ---------------------------------------------------------------------

void v99x8_device::mode_unknown(uint32_t *ln, int line)
{
	// MAME's mode_unknown() ignores its line argument; keep the signature.
	(void)line;
	const pen_t fg = pen16(m_cont_reg[7] >> 4);
	const pen_t bg = pen16(m_cont_reg[7] & 0x0f);

	for (int x = m_offset_x * 2; x > 0; x--)
		*ln++ = bg;

	for (int x = 512; x > 0; x--)
		*ln++ = fg;

	for (int x = (16 - m_offset_x) * 2; x > 0; x--)
		*ln++ = bg;
}

void v99x8_device::default_draw_sprite(uint32_t *ln, uint8_t *col)
{
	int i;
	ln += m_offset_x * 2;

	for (i=0;i<256;i++)
	{
		if (col[i] & 0x80)
		{
			*ln++ = pen16(col[i] & 0x0f);
			*ln++ = pen16(col[i] & 0x0f);
		}
		else
		{
			ln += 2;
		}
	}
}

void v99x8_device::graphic5_draw_sprite(uint32_t *ln, uint8_t *col)
{
	int i;
	ln += m_offset_x * 2;

	for (i=0;i<256;i++)
	{
		if (col[i] & 0x80)
		{
			*ln++ = pen16((col[i] >> 2) & 0x03);
			*ln++ = pen16(col[i] & 0x03);
		}
		else
		{
			ln += 2;
		}
	}
}


void v99x8_device::graphic7_draw_sprite(uint32_t *ln, uint8_t *col)
{
	static const uint16_t g7_ind16[16] = {
		0, 2, 192, 194, 48, 50, 240, 242,
	482, 7, 448, 455, 56, 63, 504, 511  };
	int i;

	ln += m_offset_x * 2;

	for (i=0;i<256;i++)
	{
		if (col[i] & 0x80)
		{
			const pen_t color = rgb3(uint8_t(g7_ind16[col[i] & 0x0f] >> 6), uint8_t((g7_ind16[col[i] & 0x0f] >> 3) & 7), uint8_t(g7_ind16[col[i] & 0x0f] & 7));
			*ln++ = color;
			*ln++ = color;
		}
		else
		{
			ln += 2;
		}
	}
}



void v99x8_device::sprite_mode1 (int line, uint8_t *col)
{
	int attrtbl_addr, patterntbl_addr, pattern_addr;
	int x, y, p, height, c, p2, i, n, pattern;

	memset(col, 0, 256);

	// are sprites disabled?
	if (m_cont_reg[8] & 0x02) return;

	attrtbl_addr = (m_cont_reg[5] << 7) + (m_cont_reg[11] << 15);
	patterntbl_addr = (m_cont_reg[6] << 11);

	// 16x16 or 8x8 sprites
	height = (m_cont_reg[1] & 2) ? 16 : 8;
	// magnified sprites (zoomed)
	if (m_cont_reg[1] & 1) height *= 2;

	p2 = p = 0;
	while (1)
	{
		y = vram_barrier(attrtbl_addr);
		if (y == 208) break;
		y = (y - m_cont_reg[23]) & 255;
		if (y > 208)
			y = -(~y&255);
		else
			y++;

		// if sprite in range, has to be drawn
		if ( (line >= y) && (line  < (y + height) ) )
		{
			if (p2 == 4)
			{
				// max maximum sprites per line!
				if ( !(m_stat_reg[0] & 0x40) )
					m_stat_reg[0] = (m_stat_reg[0] & 0xa0) | 0x40 | p;

				break;
			}
			// get x
			x = vram_barrier(attrtbl_addr + 1);
			if (vram_barrier(attrtbl_addr + 3) & 0x80) x -= 32;

			// get pattern
			pattern = vram_barrier(attrtbl_addr + 2);
			if (m_cont_reg[1] & 2)
				pattern &= 0xfc;
			n = line - y;
			pattern_addr = patterntbl_addr + pattern * 8 + ((m_cont_reg[1] & 1) ? n/2  : n);
			pattern = (vram_barrier(pattern_addr) << 8) | vram_barrier(pattern_addr+16);

			// get colour
			c = vram_barrier(attrtbl_addr + 3) & 0x0f;

			// draw left part
			n = 0;
			while (1)
			{
				if (n == 0) pattern = vram_barrier(pattern_addr);
				else if ( (n == 1) && (m_cont_reg[1] & 2) ) pattern = vram_barrier(pattern_addr + 16);
				else break;

				n++;

				for (i=0;i<8;i++)
				{
					if (pattern & 0x80)
					{
						if ( (x >= 0) && (x < 256) )
						{
							if (col[x] & 0x40)
							{
								// we have a collision!
								if (p2 < 4)
									m_stat_reg[0] |= 0x20;
							}
							if ( !(col[x] & 0x80) )
							{
								if (c || (m_cont_reg[8] & 0x20) )
									col[x] |= 0xc0 | c;
								else
									col[x] |= 0x40;
							}

							// if zoomed, draw another pixel
							if (m_cont_reg[1] & 1)
							{
								if (col[x+1] & 0x40)
								{
									// we have a collision!
									if (p2 < 4)
										m_stat_reg[0] |= 0x20;
								}
								if ( !(col[x+1] & 0x80) )
								{
									if (c || (m_cont_reg[8] & 0x20) )
										col[x+1] |= 0xc0 | c;
									else
										col[x+1] |= 0x80;
								}
							}
						}
					}
					if (m_cont_reg[1] & 1) x += 2; else x++;
					pattern <<= 1;
				}
			}

			p2++;
		}

		if (p >= 31) break;
		p++;
		attrtbl_addr += 4;
	}

	if ( !(m_stat_reg[0] & 0x40) )
		m_stat_reg[0] = (m_stat_reg[0] & 0xa0) | p;
}

void v99x8_device::sprite_mode2 (int line, uint8_t *col)
{
	int attrtbl_addr, patterntbl_addr, pattern_addr, colourtbl_addr;
	int x, i, y, p, height, c, p2, n, pattern, colourmask, first_cc_seen;

	memset(col, 0, 256);

	// are sprites disabled?
	if (m_cont_reg[8] & 0x02) return;

	attrtbl_addr = ( (m_cont_reg[5] & 0xfc) << 7) + (m_cont_reg[11] << 15);
	colourtbl_addr =  ( (m_cont_reg[5] & 0xf8) << 7) + (m_cont_reg[11] << 15);
	patterntbl_addr = (m_cont_reg[6] << 11);
	colourmask = ( (m_cont_reg[5] & 3) << 3) | 0x7; // check this!

	// 16x16 or 8x8 sprites
	height = (m_cont_reg[1] & 2) ? 16 : 8;
	// magnified sprites (zoomed)
	if (m_cont_reg[1] & 1) height *= 2;

	p2 = p = first_cc_seen = 0;
	while (1)
	{
		y = vram_read(attrtbl_addr);
		if (y == 216) break;
		y = (y - m_cont_reg[23]) & 255;
		if (y > 216)
			y = -(~y&255);
		else
			y++;

		// if sprite in range, has to be drawn
		if ( (line >= y) && (line  < (y + height) ) )
		{
			if (p2 == 8)
			{
				// max maximum sprites per line!
				if ( !(m_stat_reg[0] & 0x40) )
					m_stat_reg[0] = (m_stat_reg[0] & 0xa0) | 0x40 | p;

				break;
			}

			n = line - y; if (m_cont_reg[1] & 1) n /= 2;
			// get colour
			c = vram_read(colourtbl_addr + (((p&colourmask)*16) + n));

			// don't draw all sprite with CC set before any sprites
			// with CC = 0 are seen on this line
			if (c & 0x40)
			{
				if (!first_cc_seen)
					goto skip_first_cc_set;
			}
			else
				first_cc_seen = 1;

			// get pattern
			pattern = vram_read(attrtbl_addr + 2);
			if (m_cont_reg[1] & 2)
				pattern &= 0xfc;
			pattern_addr = patterntbl_addr + pattern * 8 + n;
			pattern = (vram_read(pattern_addr) << 8) | vram_read(pattern_addr + 16);

			// get x
			x = vram_read(attrtbl_addr + 1);
			if (c & 0x80) x -= 32;

			n = (m_cont_reg[1] & 2) ? 16 : 8;
			while (n--)
			{
				for (i=0;i<=(m_cont_reg[1] & 1);i++)
				{
					if ( (x >= 0) && (x < 256) )
					{
						if ( (pattern & 0x8000) && !(col[x] & 0x10) )
						{
							if ( (c & 15) || (m_cont_reg[8] & 0x20) )
							{
								if ( !(c & 0x40) )
								{
									if (col[x] & 0x20) col[x] |= 0x10;
									else
										col[x] |= 0x20 | (c & 15);
								}
								else
									col[x] |= c & 15;

								col[x] |= 0x80;
							}
						}
						else
						{
							if ( !(c & 0x40) && (col[x] & 0x20) )
								col[x] |= 0x10;
						}

						if ( !(c & 0x60) && (pattern & 0x8000) )
						{
							if (col[x] & 0x40)
							{
								// sprite collision!
								if (p2 < 8)
									m_stat_reg[0] |= 0x20;
							}
							else
								col[x] |= 0x40;
						}

						x++;
					}
				}

				pattern <<= 1;
			}

		skip_first_cc_set:
			p2++;
		}

		if (p >= 31) break;
		p++;
		attrtbl_addr += 4;
	}

	if ( !(m_stat_reg[0] & 0x40) )
		m_stat_reg[0] = (m_stat_reg[0] & 0xa0) | p;
}



// ---------------------------------------------------------------------
//  High-resolution bitmap modes (MAME packed/interleaved VRAM).
// ---------------------------------------------------------------------

void v99x8_device::mode_graphic5(uint32_t *ln, int line)
{
	int nametbl_addr, colour;
	int line2, linemask, x, xx;
	pen_t pen_bg0[4];
	pen_t pen_bg1[4];

	linemask = ((m_cont_reg[2] & 0x1f) << 3) | 7;

	line2 = ((line + m_cont_reg[23]) & linemask) & 255;

	nametbl_addr = ((m_cont_reg[2] & 0x40) << 10) + line2 * 128;
	if ( (m_cont_reg[2] & 0x20) && v9938_second_field() )
		nametbl_addr += 0x8000;

	pen_bg1[0] = pen16(m_cont_reg[7] & 0x03);
	pen_bg0[0] = pen16((m_cont_reg[7] >> 2) & 0x03);

	xx = m_offset_x;
	while (xx--) { *ln++ = pen_bg0[0]; *ln++ = pen_bg1[0]; }

	x = (m_cont_reg[8] & 0x20) ? 0 : 1;

	for (;x<4;x++)
	{
		pen_bg0[x] = pen16(x);
		pen_bg1[x] = pen16(x);
	}

	for (x=0;x<128;x++)
	{
		colour = vram_barrier(nametbl_addr++);

		*ln++ = pen_bg0[colour>>6];
		*ln++ = pen_bg1[(colour>>4)&3];
		*ln++ = pen_bg0[(colour>>2)&3];
		*ln++ = pen_bg1[(colour&3)];
	}

	pen_bg1[0] = pen16(m_cont_reg[7] & 0x03);
	pen_bg0[0] = pen16((m_cont_reg[7] >> 2) & 0x03);
	xx = 16 - m_offset_x;
	while (xx--) { *ln++ = pen_bg0[0]; *ln++ = pen_bg1[0]; }
}

void v99x8_device::mode_graphic6(uint32_t *ln, int line)
{
	uint8_t colour;
	int line2, linemask, x, xx, nametbl_addr;
	pen_t pen_bg, fg0;
	pen_t fg1;

	linemask = ((m_cont_reg[2] & 0x1f) << 3) | 7;

	line2 = ((line + m_cont_reg[23]) & linemask) & 255;

	nametbl_addr = line2 << 8 ;
	if ( (m_cont_reg[2] & 0x20) && v9938_second_field() )
		nametbl_addr += 0x10000;

	pen_bg = pen16(m_cont_reg[7] & 0x0f);
	xx = m_offset_x * 2;
	while (xx--) *ln++ = pen_bg;

	if (m_cont_reg[2] & 0x40)
	{
		for (x=0;x<32;x++)
		{
			nametbl_addr++;
			colour = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			fg0 = pen16(colour >> 4);
			fg1 = pen16(colour & 15);
			*ln++ = fg0; *ln++ = fg1; *ln++ = fg0; *ln++ = fg1;
			*ln++ = fg0; *ln++ = fg1; *ln++ = fg0; *ln++ = fg1;
			*ln++ = fg0; *ln++ = fg1; *ln++ = fg0; *ln++ = fg1;
			*ln++ = fg0; *ln++ = fg1; *ln++ = fg0; *ln++ = fg1;
			nametbl_addr += 7;
		}
	}
	else
	{
		for (x=0;x<256;x++)
		{
			colour = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			*ln++ = pen16(colour >> 4);
			*ln++ = pen16(colour & 15);
			nametbl_addr++;
		}
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) *ln++ = pen_bg;
}

void v99x8_device::mode_graphic7(uint32_t *ln, int line)
{
	uint8_t colour;
	int line2, linemask, x, xx, nametbl_addr;
	pen_t pen, pen_bg;

	linemask = ((m_cont_reg[2] & 0x1f) << 3) | 7;

	line2 = ((line + m_cont_reg[23]) & linemask) & 255;

	nametbl_addr = line2 << 8;
	if ( (m_cont_reg[2] & 0x20) && v9938_second_field() )
		nametbl_addr += 0x10000;

	pen_bg = pen256(m_cont_reg[7]);
	xx = m_offset_x * 2;
	while (xx--) *ln++ = pen_bg;

	// PORT: the V9958 YJK/YAE branches from v9938.cpp:1373-1438, read from the
	// lazy process-wide YJK table rather than MAME's static member array.
	if ((m_v9958_sp_mode & 0x18) == 0x08) // v9958 screen 12, puzzle star title screen
	{
		const std::vector<uint32_t> &s_pal_indYJK = yjk_palette();
		for (x=0;x<64;x++)
		{
			int colour[4];
			int ind;

			colour[0] = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			nametbl_addr++;
			colour[1] = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			nametbl_addr++;
			colour[2] = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			nametbl_addr++;
			colour[3] = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));

			ind = (colour[0] & 7) << 11 | (colour[1] & 7) << 14 |
			(colour[2] & 7) << 5 | (colour[3] & 7) << 8;

			*ln++ = s_pal_indYJK[ind | ((colour[0] >> 3) & 31)];
			*ln++ = s_pal_indYJK[ind | ((colour[0] >> 3) & 31)];

			*ln++ = s_pal_indYJK[ind | ((colour[1] >> 3) & 31)];
			*ln++ = s_pal_indYJK[ind | ((colour[1] >> 3) & 31)];

			*ln++ = s_pal_indYJK[ind | ((colour[2] >> 3) & 31)];
			*ln++ = s_pal_indYJK[ind | ((colour[2] >> 3) & 31)];

			*ln++ = s_pal_indYJK[ind | ((colour[3] >> 3) & 31)];
			*ln++ = s_pal_indYJK[ind | ((colour[3] >> 3) & 31)];

			nametbl_addr++;
		}
	}
	else if ((m_v9958_sp_mode & 0x18) == 0x18) // v9958 screen 10/11, puzzle star & sexy boom gameplay
	{
		const std::vector<uint32_t> &s_pal_indYJK = yjk_palette();
		for (x=0;x<64;x++)
		{
			int colour[4];
			int ind;

			colour[0] = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			nametbl_addr++;
			colour[1] = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			nametbl_addr++;
			colour[2] = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			nametbl_addr++;
			colour[3] = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));

			ind = (colour[0] & 7) << 11 | (colour[1] & 7) << 14 |
			(colour[2] & 7) << 5 | (colour[3] & 7) << 8;

			*ln++ = colour[0] & 8 ? pen16(colour[0] >> 4) : s_pal_indYJK[ind | ((colour[0] >> 3) & 30)];
			*ln++ = colour[0] & 8 ? pen16(colour[0] >> 4) : s_pal_indYJK[ind | ((colour[0] >> 3) & 30)];

			*ln++ = colour[1] & 8 ? pen16(colour[1] >> 4) : s_pal_indYJK[ind | ((colour[1] >> 3) & 30)];
			*ln++ = colour[1] & 8 ? pen16(colour[1] >> 4) : s_pal_indYJK[ind | ((colour[1] >> 3) & 30)];

			*ln++ = colour[2] & 8 ? pen16(colour[2] >> 4) : s_pal_indYJK[ind | ((colour[2] >> 3) & 30)];
			*ln++ = colour[2] & 8 ? pen16(colour[2] >> 4) : s_pal_indYJK[ind | ((colour[2] >> 3) & 30)];

			*ln++ = colour[3] & 8 ? pen16(colour[3] >> 4) : s_pal_indYJK[ind | ((colour[3] >> 3) & 30)];
			*ln++ = colour[3] & 8 ? pen16(colour[3] >> 4) : s_pal_indYJK[ind | ((colour[3] >> 3) & 30)];

			nametbl_addr++;
		}
	}
	else if (m_cont_reg[2] & 0x40)
	{
		for (x=0;x<32;x++)
		{
			nametbl_addr++;
			colour = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			pen = pen256(colour);
			*ln++ = pen; *ln++ = pen;
			*ln++ = pen; *ln++ = pen;
			*ln++ = pen; *ln++ = pen;
			*ln++ = pen; *ln++ = pen;
			*ln++ = pen; *ln++ = pen;
			*ln++ = pen; *ln++ = pen;
			*ln++ = pen; *ln++ = pen;
			*ln++ = pen; *ln++ = pen;
			nametbl_addr++;
		}
	}
	else
	{
		for (x=0;x<256;x++)
		{
			colour = vram_barrier(((nametbl_addr&1) << 16) | (nametbl_addr>>1));
			pen = pen256(colour);
			*ln++ = pen;
			*ln++ = pen;
			nametbl_addr++;
		}
	}

	xx = (16 - m_offset_x) * 2;
	while (xx--) *ln++ = pen_bg;
}
} // namespace srz80::vdp
