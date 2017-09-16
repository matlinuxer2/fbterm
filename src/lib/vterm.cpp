/*
 *   Copyright � 2008 dragchan <zgchan317@gmail.com>
 *   based on GTerm by Timothy Miller <tim@techsource.com>
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <string.h>
#include "vterm.h"
#include "io.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

const VTerm::CharAttr VTerm::default_char_attr = { -1, -1, 1, 0, 0, 0, 0, VTerm::CharAttr::Single };

VTerm::CharAttr VTerm::normal_char_attr()
{
	CharAttr a(char_attr);
	if (a.underline && default_underline_color != -1) {
		a.underline = false;
		a.fcolor = default_underline_color;
	}

	if (a.intensity == 0 && default_halfbright_color != -1) {
		a.intensity = 1;
		a.fcolor = default_halfbright_color;
	}

	return a;
}

VTerm::CharAttr VTerm::erase_char_attr()
{
	CharAttr a(default_char_attr);
	a.fcolor = char_attr.fcolor;
	a.bcolor = char_attr.bcolor;
	a.blink = char_attr.blink;

	return a;
}

VTerm::ModeFlag::ModeFlag()
{
	bzero(this, sizeof(ModeFlag));
	auto_wrap = true;
	cursor_visible = true;
	autorepeat_key = true;
}

u16 VTerm::history_lines;
const VTerm::StateOption *VTerm::hash_control_state[256];
const VTerm::StateOption *VTerm::hash_esc_state[128];
const VTerm::StateOption *VTerm::hash_square_state[128];

void VTerm::init_state()
{
	#define INIT_STATE_TABLE(a) \
	do { \
		bzero(hash_##a, sizeof(hash_##a)); \
		for (u32 i = 0; a[i].key != (u16)-1; i++) { \
			for (u32 j = 0; j <= (a[i].key >> 8); j++) { \
				hash_##a[(a[i].key & 0xff) + j] = a + i; \
			} \
		} \
	} while (0)

	INIT_STATE_TABLE(control_state);
	INIT_STATE_TABLE(esc_state);
	INIT_STATE_TABLE(square_state);
}

VTerm::VTerm(u16 w, u16 h)
{
	static bool inited = false;
	if (!inited) {
		inited = true;
		init_state();
		history_lines = init_history_lines();
	}

	text = 0;
	attrs = 0;
	tab_stops = 0;
	linenumbers = 0;
	dirty_startx = 0;
	dirty_endx = 0;

	width = height = 0;
	max_width = max_height = 0;

	history_full = false;
	history_save_line = 0;
	visual_start_line = 0;

	reset();
	resize(w, h);
}

VTerm::~VTerm()
{
	if (!text) return;

	delete[] text;
	delete[] attrs;
	delete[] tab_stops;
	delete[] linenumbers;
	delete[] dirty_startx;
	delete[] dirty_endx;
}

void VTerm::reset()
{
	utf8 = !strcasecmp(IoPipe::localCodec(), "UTF-8");
	utf8_count = 0;
//	g0_is_active = true;
	normal_state = true;

	pending_scroll = 0;
	scroll_top = 0;
	scroll_bot = height ? (height - 1) : 0;
	cursor_x = cursor_y = 0;
	s_cursor_x = s_cursor_y = 0;

	mode_flags = ModeFlag();
	char_attr = s_char_attr = default_char_attr;
	default_fcolor = default_bcolor = -1;
	default_underline_color = default_halfbright_color = -1;

	if (text) {
		bzero(tab_stops, max_width / 8 + 1);
		clear_area(0, 0, width - 1, height - 1);
	}
}

void VTerm::resize(u16 w, u16 h)
{
	if (!w || !h || (w == width && h == height)) return;

	u16 new_max_width = (w > max_width) ? w : max_width;
	u16 new_max_height = (h > max_height) ? h : max_height;
	u16 minw = MIN(width, w), minh = MIN(height, h);

	if (new_max_width > max_width) {
		s8 *new_tab_stops = new s8[new_max_width / 8 + 1];
		bzero(new_tab_stops, new_max_width / 8 + 1);

		if (tab_stops) {
			memcpy(new_tab_stops, tab_stops, max_width / 8 + 1);
			delete[] tab_stops;
		}
		tab_stops = new_tab_stops;
	} else if (w > width) {
		
	}

	if (new_max_height > max_height) {
		u16 *new_linenumbers = new u16[new_max_height];
		u16 *new_dirty_startx = new u16[new_max_height];
		u16 *new_dirty_endx = new u16[new_max_height];

		for (u16 i = 0; i < new_max_height; i++) {
			bool orig = (linenumbers && i < height);
			new_linenumbers[i] = orig ? linenumbers[i] : (history_lines + i);
			new_dirty_startx[i] = orig ? dirty_startx[i] : w;
			new_dirty_endx[i] = orig ? dirty_endx[i] : 0;
		}

		if (linenumbers) {
			delete[] linenumbers;
			delete[] dirty_startx;
			delete[] dirty_endx;
		}

		linenumbers = new_linenumbers;
		dirty_startx = new_dirty_startx;
		dirty_endx = new_dirty_endx;
	}

	if (new_max_width > max_width || new_max_height > max_height) {
		u16 *new_text = new u16[new_max_width * (history_lines + new_max_height)];
		CharAttr *new_attrs = new CharAttr[new_max_width * (history_lines + new_max_height)];

		if (text) {
			u32 start, new_start;
			u16 history_copy_lines = history_full ? history_lines : history_save_line;
			for (u16 i = 0; i < history_copy_lines; i++) {
				start = i * max_width;
				new_start = i * new_max_width;
				memcpy(&new_text[new_start], &text[start], sizeof(*text) * max_width);
				memcpy(&new_attrs[new_start], &attrs[start], sizeof(*attrs) * max_width);

				for (u16 j = max_width; j < new_max_width; j++) {
					new_text[new_start + j] = 0x20;
					new_attrs[new_start + j] = default_char_attr;
				}
			}

			for (u16 i = 0; i < max_height; i++) {
				start = (history_lines + i) * max_width;
				new_start = (history_lines + i) * new_max_width;
				memcpy(&new_text[new_start], &text[start], sizeof(*text) * max_width);
				memcpy(&new_attrs[new_start], &attrs[start], sizeof(*attrs) * max_width);
			}

			delete[] text;
			delete[] attrs;
		}

		text = new_text;
		attrs = new_attrs;
		max_width = new_max_width;
		max_height = new_max_height;
	}

	bool h_changed = false;
	if (h != height) {
		h_changed = true;

		if (h < height) {
			if (h <= cursor_y) {
				scroll_region(0, height - 1, cursor_y - h + 1);
				move_cursor(cursor_x, h - 1);
			}
		}
	}

	if (w <= cursor_x) {
		move_cursor(w - 1, cursor_y);
	}

	width = w;
	height = h;
	
	if (h_changed) {
		historyChange(visual_start_line, total_history_lines());
	}
	
	scroll_bot = height - 1;
	if (scroll_top >= height) scroll_top = 0;

	if (minw < w) {
		clear_area(minw, 0, w - 1, h - 1);
	}

	if (minh < h) {
		clear_area(0, minh, minw, h - 1);
	}

}

void VTerm::input(const u8 *buf, u32 count)
{
	if (!width) return;

	if (visual_start_line != total_history_lines()) {
		historyDisplay(true, total_history_lines());
		historyChange(visual_start_line, total_history_lines());
	}

	u32 c, tc;
	bool rescan;

	while (count > 0) {
		u32 orig = *buf;
		c = orig;
		buf++;
		count--;
		rescan = 0;

		/* Do no translation at all in control states */
		if (!normal_state) {
			tc = c;
		} else { // if (utf8 && !mode_flags.display_ctrl) {
			rescan_last_byte:
			if ((c & 0xc0) == 0x80) {
				/* Continuation byte received */
				static const u32 utf8_length_changes[] = { 0x0000007f, 0x000007ff, 0x0000ffff, 0x001fffff, 0x03ffffff, 0x7fffffff};
				if (utf8_count) {
					cur_char = (cur_char << 6) | (c & 0x3f);
					npar++;
					if (--utf8_count) {
						/* Still need some bytes */
						continue;
					}
					/* Got a whole s8acter */
					c = cur_char;
					/* Reject overlong sequences */
					if (c <= utf8_length_changes[npar - 1] ||
						c > utf8_length_changes[npar])
						c = 0xfffd;
				} else {
					/* Unexpected continuation byte */
					utf8_count = 0;
					c = 0xfffd;
				}
			} else {
				/* Single ASCII byte or first byte of a sequence received */
				if (utf8_count) {
					/* Continuation byte expected */
					rescan = 1;
					utf8_count = 0;
					c = 0xfffd;
				} else if (c > 0x7f) {
					/* First byte of a multibyte sequence received */
					npar = 0;
					if ((c & 0xe0) == 0xc0) {
						utf8_count = 1;
						cur_char = (c & 0x1f);
					} else if ((c & 0xf0) == 0xe0) {
						utf8_count = 2;
						cur_char = (c & 0x0f);
					} else if ((c & 0xf8) == 0xf0) {
						utf8_count = 3;
						cur_char = (c & 0x07);
					} else if ((c & 0xfc) == 0xf8) {
						utf8_count = 4;
						cur_char = (c & 0x03);
					} else if ((c & 0xfe) == 0xfc) {
						utf8_count = 5;
						cur_char = (c & 0x01);
					} else {
						/* 254 and 255 are invalid */
						c = 0xfffd;
					}
					if (utf8_count) {
						/* Still need some bytes */
						continue;
					}
				}
				/* Nothing to do if an ASCII byte was received */
			}
			/* End of UTF-8 decoding. */
			/* c is the received character, or U+FFFD for invalid sequences. */
			/* Replace invalid Unicode code points with U+FFFD too */
			if ((c >= 0xd800 && c <= 0xdfff) || c == 0xfffe || c == 0xffff)
				c = 0xfffd;
			tc = c;
		} // else {	/* no utf or alternate charset mode */
			//tc = c;
		//}

		cur_char = tc;

		/* A bitmap for codes <32. A bit of 1 indicates that the code
		* corresponding to that bit number invokes some special action
		* (such as cursor movement) and should not be displayed as a
		* glyph unless the disp_ctrl mode is explicitly enabled.
		*/
#define CTRL_ACTION 0x0d00ff81
#define CTRL_ALWAYS 0x0800f501	/* Cannot be overridden by disp_ctrl */

		/* If the original code was a control character we
		 * only allow a glyph to be displayed if the code is
		 * not normally used (such as for cursor movement) or
		 * if the disp_ctrl mode has been explicitly enabled.
		 * Certain characters (as given by the CTRL_ALWAYS
		 * bitmap) are always displayed as control characters,
		 * as the console would be pretty useless without
		 * them; to display an arbitrary font position use the
		 * direct-to-font zone in UTF-8 mode.
		 */
		bool ok = tc && (c >= 32 || !(mode_flags.display_ctrl ? (CTRL_ALWAYS >> c) & 1 : utf8 || ((CTRL_ACTION >> c) & 1)))
				  && (c != 127 || mode_flags.display_ctrl)
				  && (c != 128+27);

		if (normal_state && ok) {
			if (c == 0xfeff || (c >= 0x200b && c <= 0x200f)) { // zero width 
				continue;
			}

			do_normal_char();

			if (rescan) {
				rescan = 0;
				c = orig;
				goto rescan_last_byte;
			}
			continue;
		}

		do_control_char();
	}

	update();
}

/* is_double_width() is based on the wcwidth() implementation by
 * Markus Kuhn -- 2007-05-26 (Unicode 5.0)
 * Latest version: http://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c
 */
struct interval {
	u32 first;
	u32 last;
};

static bool bisearch(u32 ucs, const struct interval *table, u32 max)
{
	u32 min = 0;
	u32 mid;

	if (ucs < table[0].first || ucs > table[max].last)
		return 0;
	while (max >= min) {
		mid = (min + max) / 2;
		if (ucs > table[mid].last)
			min = mid + 1;
		else if (ucs < table[mid].first)
			max = mid - 1;
		else
			return 1;
	}
	return 0;
}

static bool is_double_width(u32 ucs)
{
	static const struct interval double_width[] = {
		{ 0x1100, 0x115F}, { 0x2329, 0x232A}, { 0x2E80, 0x303E},
		{ 0x3040, 0xA4CF}, { 0xAC00, 0xD7A3}, { 0xF900, 0xFAFF},
		{ 0xFE10, 0xFE19}, { 0xFE30, 0xFE6F}, { 0xFF00, 0xFF60},
		{ 0xFFE0, 0xFFE6}, { 0x20000, 0x2FFFD}, { 0x30000, 0x3FFFD}
	};
	return bisearch(ucs, double_width, sizeof(double_width) / sizeof(struct interval) - 1);
}

void VTerm::do_normal_char()
{
	bool dw = is_double_width(cur_char);
	if (cur_char > 0xffff) cur_char = 0xfffd;

	u32 yp = linenumbers[cursor_y] * max_width;
	if ((!dw && cursor_x >= width) || (dw && (cursor_x >= width - 1))) {
		if (mode_flags.auto_wrap) {
			if (dw && cursor_x == width - 1) {
				text[yp + cursor_x] = 0x20;
				attrs[yp + cursor_x] = erase_char_attr();
			}
			next_line();
			yp = linenumbers[cursor_y] * max_width;
		} else {
			if (!dw) {
				cursor_x = width - 1;
			} else {
				return;
			}
		}
	}

	if (mode_flags.insert_mode) {
		changed_line(cursor_y, cursor_x, width - 1);

		u16 step = dw ? 2 : 1;
		for (u16 i = cursor_x; i < width - step; i++) {
			text[yp + i + step] = text[yp + i];
			attrs[yp + i + step] = attrs[yp + i];
		}
	} else {
		changed_line(cursor_y, cursor_x, cursor_x + (dw ? 1 : 0));
	}

	text[yp + cursor_x] = cur_char;
	attrs[yp + cursor_x] = normal_char_attr();

	if (dw) {
		attrs[yp + cursor_x++].type = CharAttr::DoubleLeft;

		text[yp + cursor_x] = cur_char;
		attrs[yp + cursor_x] = normal_char_attr();
		attrs[yp + cursor_x++].type = CharAttr::DoubleRight;
	} else {
		attrs[yp + cursor_x++].type = CharAttr::Single;
	}
}

void VTerm::do_control_char()
{
	const StateOption *so = 0;

	#define DO_ACTION() \
	do { \
		if (so->action) { \
			(this->*(so->action))(); \
		} \
		 \
		if (so->next_state == (StateOption*)-1) { \
			normal_state = true; \
		} else if (so->next_state) { \
			normal_state = false; \
			current_state = so->next_state; \
		} \
	} while (0)

	if (cur_char < 256) so = hash_control_state[cur_char];
	if (so) DO_ACTION();

	if (so || normal_state) return;

	if (current_state == esc_state || current_state == square_state) {
		if (cur_char < 128) {
			so = ((current_state == esc_state) ? hash_esc_state : hash_square_state)[cur_char];
		}
	} else {
		for (const StateOption *cur = current_state; cur->key != (u16)-1; cur++) {
			u32 key = cur->key & 0xff;
			u32 same = cur->key >> 8;
			if (cur_char >= key && cur_char <= (key + same)) {
				so = cur;
				break;
			}
		}
	}

	if (so) DO_ACTION();
	else normal_state = true;
}

void VTerm::update()
{
	if (!width) return;

	// first perform scroll-copy
	s32 mx = scroll_bot - scroll_top + 1;
	if (pending_scroll && pending_scroll< mx && -pending_scroll < mx) {
		u16 sy, dy, h;
		if (pending_scroll < 0) {
			sy = scroll_top;
			dy = scroll_top - pending_scroll;
			h = scroll_bot - scroll_top + pending_scroll + 1;
		} else {
			sy = scroll_top + pending_scroll;
			dy = scroll_top;
			h = scroll_bot - scroll_top - pending_scroll + 1;
		}

		if (!moveChars(0, sy, 0, dy, width, h)) {
			for (; h--; dy++) {
				if (dy >= height) break;
				dirty_startx[dy] = 0;
				dirty_endx[dy] = width - 1;
			}
		}
	}
	pending_scroll = 0;

	for (u16 i = 0; i < height; i++) {
		if (dirty_endx[i] >= dirty_startx[i]) {
			requestUpdate(dirty_startx[i], i, dirty_endx[i] - dirty_startx[i] + 1, 1);
			dirty_startx[i] = width;
			dirty_endx[i] = 0;
		}
	}

	draw_cursor();
}

void VTerm::draw_cursor()
{
	if (!mode_flags.cursor_visible) return;
	
	u32 yp = linenumbers[cursor_y] * max_width + cursor_x;

	CharAttr attr = attrs[yp];
	attr.reverse ^= mode_flags.inverse_screen;

	drawCursor(attr, cursor_x, cursor_y, text[yp]);
}

void VTerm::requestUpdate(u16 x, u16 y, u16 w, u16 h)
{
	expose(x, y, w, h);
}

void VTerm::expose(u16 x, u16 y, u16 w, u16 h)
{
	if (!width || !w || !h || x >= width || y >= height) return;

	if (x + w > width) w = width - x;
	if (y + h > height) h = height - y;

	for (; h--; y++) {
		u32 yp = get_line(y) * max_width;

		u16 startx = x;
		u16 endx = x + w - 1;

		if (attrs[yp + startx].type == CharAttr::DoubleRight) startx--;
		if (attrs[yp + endx].type == CharAttr::DoubleLeft) endx++;

		CharAttr attr = attrs[yp + startx];
		bool dws[width];
		u16 codes[width], num = 0;
		u16 cur, start = startx;
		
		for (cur = startx; cur <= endx; cur++) {
			if (attrs[yp + cur].type == CharAttr::DoubleRight) continue;
		
			if (attrs[yp + cur] != attr) {
				attr.reverse ^= mode_flags.inverse_screen;
				drawChars(attr, start, y, num, codes, dws);

				num = 0;
				start = cur;
				attr = attrs[yp + cur];
			}

			dws[num] = (attrs[yp + cur].type != CharAttr::Single);
			codes[num++] = text[yp + cur];
		}

		attr.reverse ^= mode_flags.inverse_screen;
		drawChars(attr, start, y, num, codes, dws);
	}
}

void VTerm::scroll_region(u16 start_y, u16 end_y, s16 num)
{
	s32 y, takey, fast_scroll, mx, clr;
	u16 temp[height];
	u16 temp_sx[height], temp_ex[height];

	if (!num) return;
	if (end_y >= height) end_y = height - 1;
	if (start_y > end_y) return;

	mx = end_y-start_y+1;
	if (num > mx) num = mx;
	if (-num > mx) num = -mx;

	if (start_y == 0 && num > 0) {
		history_scroll(num);
	}

	fast_scroll = (start_y == scroll_top && end_y == scroll_bot);

	if (fast_scroll) pending_scroll += num;

	memcpy(temp, linenumbers, sizeof(temp));
	if (fast_scroll) {
		memcpy(temp_sx, dirty_startx, sizeof(temp_sx));
		memcpy(temp_ex, dirty_endx, sizeof(temp_ex));
	}

	// move the lines by renumbering where they point to
	if (num<mx && -num<mx) {
		for (y=start_y; y<=end_y; y++) {
			takey = y+num;
			clr = (takey<start_y) || (takey>end_y);
			if (takey<start_y) takey = end_y+1-(start_y-takey);
			if (takey>end_y) takey = start_y-1+(takey-end_y);

			linenumbers[y] = temp[takey];

			if (!fast_scroll || clr) {
				dirty_startx[y] = 0;
				dirty_endx[y] = width-1;
			} else {
				dirty_startx[y] = temp_sx[takey];
				dirty_endx[y] = temp_ex[takey];
			}
			if (clr) {
				clear_area(0, y, width - 1, y);
			}
		}
	}
}

void VTerm::shift_text(u16 y, u16 start_x, u16 end_x, s16 num)
{
	if (!num) return;

	u32 yp = linenumbers[y]*max_width;

	u16 mx = end_x-start_x+1;
	if (num>mx)	num = mx;
	if (-num>mx) num = -mx;

	if (num<mx && -num<mx) {
		if (num<0) {
			memmove(text+yp+start_x, text+yp+start_x-num, sizeof(*text) * (mx+num));
			memmove(attrs+yp+start_x, attrs+yp+start_x-num, sizeof(*attrs) * (mx+num));
		} else {
			memmove(text+yp+start_x+num, text+yp+start_x, sizeof(*text) * (mx-num));
			memmove(attrs+yp+start_x+num, attrs+yp+start_x, sizeof(*attrs) * (mx-num));
		}
	}

	u16 x = (num < 0) ? (num + end_x + 1) : start_x;
	if (num < 0) num = -num;
	for (; num--; x++) {
		text[yp + x] = 0x20;
		attrs[yp + x] = erase_char_attr();
	}

	changed_line(y, start_x, end_x);
}

void VTerm::clear_area(u16 start_x, u16 start_y, u16 end_x, u16 end_y)
{
	if (start_x >= width || start_y >= height) return;
	if (end_x >= width) end_x = width - 1;
	if (end_y >= height) end_y = height - 1;
	if (start_x > end_x || start_y > end_y) return;

	u16 x, y;
	u32 yp;
	for (y=start_y; y<=end_y; y++) {
		yp = linenumbers[y]*max_width;
		for (x=start_x; x<=end_x; x++) {
			text[yp+x]= 0x20;
			attrs[yp+x] = erase_char_attr();
		}
		changed_line(y, start_x, end_x);
	}
}

void VTerm::changed_line(u16 y, u16 start_x, u16 end_x)
{
	if (y >= height) return;

	if (start_x >= width) start_x = width - 1;
	if (end_x >= width) end_x = width - 1;

	if (dirty_startx[y] > start_x) dirty_startx[y] = start_x;
	if (dirty_endx[y] < end_x) dirty_endx[y] = end_x;
}

void VTerm::move_cursor(u16 x, u16 y)
{
	changed_line(cursor_y, cursor_x, cursor_x);
	
	if (x>=width) x = width-1;
	if (y>=height) y = height-1;
	cursor_x = x;
	cursor_y = y;
}

bool VTerm::mode(ModeType type)
{
	bool ret = false;
	switch (type) {
	case AutoRepeatKey:
		ret = mode_flags.autorepeat_key;
		break;
	case NumericKeypad:
		ret = mode_flags.numeric_keypad;
		break;
	case X10MouseReport:
		ret = mode_flags.x10_mouse_report;
		break;
	case X11MouseReport:
		ret = mode_flags.x11_mouse_report;
		break;
	case CursorKeyEsc0:
		ret = mode_flags.cursorkey_esc0;
		break;
	case CursorVisible:
		ret = mode_flags.cursor_visible && (visual_start_line == total_history_lines());
		break;
	default:
		break;
	}

	return ret;
}

void VTerm::history_scroll(u16 num)
{
	if (!history_lines) return;

	u32 yp, yp_history;
	for (u16 i = 0; i < num; i++) {
		yp = linenumbers[i] * max_width;
		yp_history = history_save_line * max_width;
		memcpy(&text[yp_history], &text[yp], sizeof(*text) * width);
		memcpy(&attrs[yp_history], &attrs[yp], sizeof(*attrs) * width);

		if (width < max_width) {
			for (u16 i = width; i < max_width; i++) {
				text[yp_history + i] = 0x20;
				attrs[yp_history + i] = default_char_attr;
			}
		}

		history_save_line++;
		if (history_save_line == history_lines) {
			history_save_line = 0;
			if (!history_full) history_full = true;
		}
	}

	visual_start_line = total_history_lines();
	historyChange(visual_start_line, total_history_lines());
}

void VTerm::historyDisplay(bool absolute, s32 num)
{
	if (!history_lines || (absolute && num == (s32)visual_start_line) || (!absolute && !num)) return;

	u32 bak_line = visual_start_line;

	if (absolute) {
		visual_start_line = num;
	} else {
		if ((s32)visual_start_line < -num) visual_start_line = 0;
		else visual_start_line += num;
	}

	if (visual_start_line > total_history_lines()) visual_start_line = total_history_lines();
	if (visual_start_line == bak_line) return;
	
	modeChange(CursorVisible);

	bool accel_scroll = false;

	u32 scroll_lines = (visual_start_line > bak_line) ? (visual_start_line - bak_line) : (bak_line - visual_start_line);
	if (scroll_lines < height) {
		u16 sy, dy, updatey;

		if (visual_start_line > bak_line) {
			sy = scroll_lines;
			dy = 0;
			updatey = height - scroll_lines;
		} else {
			sy = 0;
			dy = scroll_lines;
			updatey = 0;
		}

		accel_scroll = moveChars(0, sy, 0, dy, width, height - scroll_lines);
		if (accel_scroll) {
			requestUpdate(0, updatey, width, scroll_lines);
		}
	}

	if (!accel_scroll) {
		requestUpdate(0, 0, width, height);
	}
}

u16 VTerm::get_line(u16 y)
{
	if (y > height) y = height;
	u16 line = visual_start_line + y;

	if (line >= total_history_lines()) return linenumbers[line - total_history_lines()];

	return ((history_full ? history_save_line : 0) + line) % history_lines;
}
