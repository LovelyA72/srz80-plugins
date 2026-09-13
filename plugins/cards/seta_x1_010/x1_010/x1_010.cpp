/*
	License: Zlib
	see https://gitlab.com/cam900/vgsound_emu/-/blob/main/LICENSE for more details

	Copyright holder(s): cam900
	Seta/Allumer X1-010 Emulation core
*/

#include "x1_010.hpp"

namespace {
void put_u16(vgsound_emu::u8 *&out, vgsound_emu::u16 value)
{
	*out++ = vgsound_emu::u8(value);
	*out++ = vgsound_emu::u8(value >> 8);
}

void put_u32(vgsound_emu::u8 *&out, vgsound_emu::u32 value)
{
	put_u16(out, vgsound_emu::u16(value));
	put_u16(out, vgsound_emu::u16(value >> 16));
}

vgsound_emu::u16 get_u16(const vgsound_emu::u8 *&in)
{
	const auto lo = *in++;
	const auto hi = *in++;
	return vgsound_emu::u16(lo) | (vgsound_emu::u16(hi) << 8);
}

vgsound_emu::u32 get_u32(const vgsound_emu::u8 *&in)
{
	return vgsound_emu::u32(get_u16(in)) | (vgsound_emu::u32(get_u16(in)) << 16);
}
} // namespace

void x1_010_core::tick()
{
	// reset output
	m_out[0] = m_out[1] = 0;
	for (voice_t &elem : m_voice)
	{
		elem.tick();
		m_out[0] += elem.out(0);
		m_out[1] += elem.out(1);
	}
}

void x1_010_core::voice_t::tick()
{
	m_out[0] = m_out[1] = 0;
	if (m_flag.keyon())
	{
		if (m_flag.wavetable())	 // Wavetable
		{
			// envelope, each nibble is for each output
			u8 vol =
			  m_host.m_envelope[(bitfield(m_end_envshape, 0, 5) << 7) | bitfield(m_env_acc, 10, 7)];
			m_vol_out[0] = bitfield(vol, 4, 4);
			m_vol_out[1] = bitfield(vol, 0, 4);
			m_env_acc	 += m_start_envfreq;
			if (m_flag.env_oneshot() && bitfield(m_env_acc, 17))
			{
				m_flag.set_keyon(false);
			}
			else
			{
				m_env_acc = bitfield(m_env_acc, 0, 17);
			}
			// get wavetable data
			m_data = m_host.m_wave[(bitfield(m_vol_wave, 0, 5) << 7) | bitfield(m_acc, 11, 7)];
			m_acc  = bitfield(m_acc + (m_freq << (1 - m_flag.div())), 0, 18);
		}
		else  // PCM sample
		{
			// volume register, each nibble is for each output
			m_vol_out[0] = bitfield(m_vol_wave, 4, 4);
			m_vol_out[1] = bitfield(m_vol_wave, 0, 4);
			// get PCM sample
			m_data = m_host.m_intf.read_byte(bitfield(m_acc, 5, 20));
			m_acc  += u32(bitfield(m_freq, 0, 8)) << (1 - m_flag.div());
			if ((m_acc >> 17) > u32(0xff ^ m_end_envshape))
			{
				m_flag.set_keyon(false);
			}
		}
		m_out[0] = m_data * m_vol_out[0];
		m_out[1] = m_data * m_vol_out[1];
	}
}

u8 x1_010_core::ram_r(u16 offset)
{
	if (offset & 0x1000)
	{  // wavetable data
		return m_wave[offset & 0xfff];
	}
	else if (offset & 0xf80)
	{  // envelope shape data
		return m_envelope[offset & 0xfff];
	}
	else
	{  // channel register
		return m_voice[bitfield(offset, 3, 4)].reg_r(offset & 0x7);
	}
}

void x1_010_core::ram_w(u16 offset, u8 data)
{
	if (offset & 0x1000)
	{  // wavetable data
		m_wave[offset & 0xfff] = data;
	}
	else if (offset & 0xf80)
	{  // envelope shape data
		m_envelope[offset & 0xfff] = data;
	}
	else
	{  // channel register
		m_voice[bitfield(offset, 3, 4)].reg_w(offset & 0x7, data);
	}
}

u8 x1_010_core::voice_t::reg_r(u8 offset)
{
	switch (offset & 0x7)
	{
		case 0x00:
			return (m_flag.div() << 7) | (m_flag.env_oneshot() << 2) | (m_flag.wavetable() << 1) |
				   (m_flag.keyon() << 0);
		case 0x01: return m_vol_wave;
		case 0x02: return bitfield(m_freq, 0, 8);
		case 0x03: return bitfield(m_freq, 8, 8);
		case 0x04: return m_start_envfreq;
		case 0x05: return m_end_envshape;
		default: break;
	}
	return 0;
}

void x1_010_core::voice_t::reg_w(u8 offset, u8 data)
{
	switch (offset & 0x7)
	{
		case 0x00:
			{
				const bool prev_keyon = m_flag.keyon();
				m_flag.write(data);
				if (!prev_keyon && m_flag.keyon())	// Key on
				{
					m_acc	  = m_flag.wavetable() ? 0 : (u32(m_start_envfreq) << 17);
					m_env_acc = 0;
				}
				break;
			}
		case 0x01: m_vol_wave = data; break;
		case 0x02: m_freq = (m_freq & 0xff00) | data; break;
		case 0x03: m_freq = (m_freq & 0x00ff) | (u16(data) << 8); break;
		case 0x04: m_start_envfreq = data; break;
		case 0x05: m_end_envshape = data; break;
		default: break;
	}
}

void x1_010_core::voice_t::reset()
{
	m_flag.reset();
	m_vol_wave		= 0;
	m_freq			= 0;
	m_start_envfreq = 0;
	m_end_envshape	= 0;
	m_acc			= 0;
	m_env_acc		= 0;
	m_data			= 0;
	m_vol_out.fill(0);
	m_out.fill(0);
}

void x1_010_core::reset()
{
	for (auto &elem : m_voice)
	{
		elem.reset();
	}

	m_envelope.fill(0);
	m_wave.fill(0);
	m_out.fill(0);
}

void x1_010_core::voice_t::save_state(u8 *&out) const
{
	*out++ = m_flag.pack();
	*out++ = m_vol_wave;
	put_u16(out, m_freq);
	*out++ = m_start_envfreq;
	*out++ = m_end_envshape;
	put_u32(out, m_acc);
	put_u32(out, m_env_acc);
	*out++ = u8(m_data);
	*out++ = m_vol_out[0];
	*out++ = m_vol_out[1];
	put_u32(out, u32(m_out[0]));
	put_u32(out, u32(m_out[1]));
}

bool x1_010_core::voice_t::load_state(const u8 *&in, const u8 *end)
{
	if (end - in < static_cast<std::ptrdiff_t>(serialized_size))
		return false;
	m_flag.unpack(*in++);
	m_vol_wave = *in++;
	m_freq = get_u16(in);
	m_start_envfreq = *in++;
	m_end_envshape = *in++;
	m_acc = get_u32(in);
	m_env_acc = get_u32(in);
	m_data = s8(*in++);
	m_vol_out[0] = *in++;
	m_vol_out[1] = *in++;
	m_out[0] = s32(get_u32(in));
	m_out[1] = s32(get_u32(in));
	return true;
}

void x1_010_core::save_state(u8 *buffer, std::size_t size) const
{
	if (!buffer || size < serialized_size)
		return;
	u8 *out = buffer;
	std::copy(m_envelope.begin(), m_envelope.end(), out);
	out += m_envelope.size();
	std::copy(m_wave.begin(), m_wave.end(), out);
	out += m_wave.size();
	for (const auto &voice : m_voice)
		voice.save_state(out);
}

bool x1_010_core::load_state(const u8 *buffer, std::size_t size)
{
	if (!buffer || size != serialized_size)
		return false;
	const u8 *in = buffer;
	const u8 *end = buffer + size;
	std::copy(in, in + m_envelope.size(), m_envelope.begin());
	in += m_envelope.size();
	std::copy(in, in + m_wave.size(), m_wave.begin());
	in += m_wave.size();
	for (auto &voice : m_voice) {
		if (!voice.load_state(in, end))
			return false;
	}
	return in == end;
}
