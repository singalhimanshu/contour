/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/Sequencer.h>

#include <terminal/Functions.h>
#include <terminal/MessageParser.h>
#include <terminal/Screen.h>
#include <terminal/SixelParser.h>

#include <crispy/algorithm.h>
#include <crispy/escape.h>
#include <crispy/base64.h>
#include <crispy/utils.h>

#include <unicode/utf8.h>

#include <fmt/format.h>

#include <array>
#include <iostream>             // error logging
#include <cassert>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <sstream>
#include <vector>

using std::array;
using std::get;
using std::holds_alternative;
using std::make_shared;
using std::make_unique;
using std::min;
using std::nullopt;
using std::optional;
using std::pair;
using std::runtime_error;
using std::shared_ptr;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

#define CONTOUR_SYNCHRONIZED_OUTPUT 1

namespace terminal {

namespace // {{{ helpers
{
    /// @returns parsed tuple with OSC code and offset to first data parameter byte.
    pair<int, int> parseOSC(string const& _data)
    {
        int code = 0;
        size_t i = 0;

        while (i < _data.size() && isdigit(_data[i]))
            code = code * 10 + _data[i++] - '0';

        if (i == 0 && !_data.empty() && _data[0] != ';')
        {
            // such as 'L' is encoded as -'L'
            code = -_data[0];
            ++i;
        }

        if (i < _data.size() && _data[i] == ';')
            ++i;

        return pair{code, i};
    }

    // optional<CharsetTable> getCharsetTableForCode(std::string const& _intermediate)
    // {
    //     if (_intermediate.size() != 1)
    //         return nullopt;
    //
    //     char32_t const code = _intermediate[0];
    //     switch (code)
    //     {
    //         case '(':
    //             return {CharsetTable::G0};
    //         case ')':
    //         case '-':
    //             return {CharsetTable::G1};
    //         case '*':
    //         case '.':
    //             return {CharsetTable::G2};
    //         case '+':
    //         case '/':
    //             return {CharsetTable::G3};
    //         default:
    //             return nullopt;
    //     }
    // }
} // }}}

namespace impl // {{{ some command generator helpers
{
    ApplyResult setAnsiMode(Sequence const& _seq, size_t _modeIndex, bool _enable, Screen& _screen)
	{
		switch (_seq.param(_modeIndex))
		{
			case 2:  // (AM) Keyboard Action Mode
				return ApplyResult::Unsupported;
			case 4:  // (IRM) Insert Mode
                _screen.setMode(Mode::Insert, _enable);
                return ApplyResult::Ok;
			case 12:  // (SRM) Send/Receive Mode
			case 20:  // (LNM) Automatic Newline
			default:
				return ApplyResult::Unsupported;
		}
	}

    optional<Mode> toDECMode(int _value)
    {
        switch (_value)
        {
            case 1: return Mode::UseApplicationCursorKeys;
            case 2: return Mode::DesignateCharsetUSASCII;
            case 3: return Mode::Columns132;
            case 4: return Mode::SmoothScroll;
            case 5: return Mode::ReverseVideo;
            case 6: return Mode::Origin;
            case 7: return Mode::AutoWrap;
            // TODO: Ps = 8  -> Auto-repeat Keys (DECARM), VT100.
            case 9: return Mode::MouseProtocolX10;
            case 10: return Mode::ShowToolbar;
            case 12: return Mode::BlinkingCursor;
            case 19: return Mode::PrinterExtend;
            case 25: return Mode::VisibleCursor;
            case 30: return Mode::ShowScrollbar;
            // TODO: Ps = 3 5  -> Enable font-shifting functions (rxvt).
            // IGNORE? Ps = 3 8  -> Enter Tektronix Mode (DECTEK), VT240, xterm.
            // TODO: Ps = 4 0  -> Allow 80 -> 132 Mode, xterm.
            case 40: return Mode::AllowColumns80to132;
            // IGNORE: Ps = 4 1  -> more(1) fix (see curses resource).
            // TODO: Ps = 4 2  -> Enable National Replacement Character sets (DECNRCM), VT220.
            // TODO: Ps = 4 4  -> Turn On Margin Bell, xterm.
            // TODO: Ps = 4 5  -> Reverse-wraparound Mode, xterm.
            case 47: return Mode::UseAlternateScreen;
            // TODO: Ps = 6 6  -> Application keypad (DECNKM), VT320.
            // TODO: Ps = 6 7  -> Backarrow key sends backspace (DECBKM), VT340, VT420.  This sets the backarrowKey resource to "true".
            case 69: return Mode::LeftRightMargin;
            case 80: return Mode::SixelScrolling;
            case 1000: return Mode::MouseProtocolNormalTracking;
            case 1001: return Mode::MouseProtocolHighlightTracking;
            case 1002: return Mode::MouseProtocolButtonTracking;
            case 1003: return Mode::MouseProtocolAnyEventTracking;
            case 1004: return Mode::FocusTracking;
            case 1005: return Mode::MouseExtended;
            case 1006: return Mode::MouseSGR;
            case 1007: return Mode::MouseAlternateScroll;
            case 1015: return Mode::MouseURXVT;
            case 1047: return Mode::UseAlternateScreen;
            case 1048: return Mode::SaveCursor;
            case 1049: return Mode::ExtendedAltScreen;
            case 2004: return Mode::BracketedPaste;
            case 2026: return Mode::BatchedRendering;
        }
        return nullopt;
    }

	ApplyResult setModeDEC(Sequence const& _seq, size_t _modeIndex, bool _enable, Screen& _screen)
	{
        if (auto const modeOpt = toDECMode(_seq.param(_modeIndex)); modeOpt.has_value())
        {
            _screen.setMode(modeOpt.value(), _enable);
            return ApplyResult::Ok;
        }
        return ApplyResult::Invalid;
	}

    Color parseColor(Sequence const& _seq, size_t* pi)
	{
        // We are at parameter index `i`.
        //
        // It may now follow:
        // - ":2;r;g;b"         RGB color
        // - ":3:F:C:M:Y"       CMY color  (F is scaling factor, what is max? 100 or 255?)
        // - ":4:F:C:M:Y:K"     CMYK color (F is scaling factor, what is max? 100 or 255?)
        // - ":5:P"
        // Sub-parameters can also be delimited with ';' and thus are no sub-parameters per-se.
        size_t i = *pi;
        if (_seq.subParameterCount(i) >= 1)
        {
            switch (_seq.subparam(i, 0))
            {
                case 2: // ":2:R:G:B"
                    if (_seq.subParameterCount(i) == 4)
                    {
                        auto const r = _seq.subparam(i, 1);
                        auto const g = _seq.subparam(i, 2);
                        auto const b = _seq.subparam(i, 3);
                        if (r <= 255 && g <= 255 && b <= 255)
                        {
                            *pi = i + 1;
                            return Color{RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)} };
                        }
                    }
                    break;
                case 3: // ":3:F:C:M:Y" (TODO)
                case 4: // ":4:F:C:M:Y:K" (TODO)
                    break;
                case 5: // ":5:P"
                    if (auto const P = _seq.subparam(i, 1); P <= 255)
                    {
                        *pi = i + 1;
                        return static_cast<IndexedColor>(P);
                    }
                    break;
                default:
                    break; // XXX invalid sub parameter
            }
        }

		if (i + 1 < _seq.parameterCount())
		{
			++i;
			auto const mode = _seq.param(i);
			if (mode == 5)
			{
				if (i + 1 < _seq.parameterCount())
				{
					++i;
					auto const value = _seq.param(i);
					if (i <= 255)
                    {
						*pi = i;
                        return static_cast<IndexedColor>(value);
                    }
					else
                        {} // TODO: _seq.logInvalidCSI("Invalid color indexing.");
				}
				else
                    {} // TODO: _seq.logInvalidCSI("Missing color index.");
			}
			else if (mode == 2)
			{
				if (i + 3 < _seq.parameterCount())
				{
					auto const r = _seq.param(i + 1);
					auto const g = _seq.param(i + 2);
					auto const b = _seq.param(i + 3);
					i += 3;
					if (r <= 255 && g <= 255 && b <= 255)
                    {
						*pi = i;
                        return RGBColor{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
                    }
					else
                        {} // TODO: _seq.logInvalidCSI("RGB color out of range.");
				}
				else
                    {} // TODO: _seq.logInvalidCSI("Invalid color mode.");
			}
			else
                {} // TODO: _seq.logInvalidCSI("Invalid color mode.");
		}
		else
            {} // TODO: _seq.logInvalidCSI("Invalid color indexing.");

        // failure case, skip this argument
        *pi = i + 1;
        return Color{};
	}

	ApplyResult dispatchSGR(Sequence const& _seq, Screen& _screen)
	{
        if (_seq.parameterCount() == 0)
        {
           _screen.setGraphicsRendition(GraphicsRendition::Reset);
            return ApplyResult::Ok;
        }

		for (size_t i = 0; i < _seq.parameterCount(); ++i)
		{
			switch (_seq.param(i))
			{
				case 0: _screen.setGraphicsRendition(GraphicsRendition::Reset); break;
				case 1: _screen.setGraphicsRendition(GraphicsRendition::Bold); break;
				case 2: _screen.setGraphicsRendition(GraphicsRendition::Faint); break;
				case 3: _screen.setGraphicsRendition(GraphicsRendition::Italic); break;
				case 4:
                    if (_seq.subParameterCount(i) == 1)
                    {
                        switch (_seq.subparam(i, 0))
                        {
                            case 0: _screen.setGraphicsRendition(GraphicsRendition::NoUnderline); break; // 4:0
                            case 1: _screen.setGraphicsRendition(GraphicsRendition::Underline); break; // 4:1
                            case 2: _screen.setGraphicsRendition(GraphicsRendition::DoublyUnderlined); break; // 4:2
                            case 3: _screen.setGraphicsRendition(GraphicsRendition::CurlyUnderlined); break; // 4:3
                            case 4: _screen.setGraphicsRendition(GraphicsRendition::DottedUnderline); break; // 4:4
                            case 5: _screen.setGraphicsRendition(GraphicsRendition::DashedUnderline); break; // 4:5
                            default: _screen.setGraphicsRendition(GraphicsRendition::Underline); break;
                        }
                    }
                    else
                        _screen.setGraphicsRendition(GraphicsRendition::Underline);
					break;
				case 5: _screen.setGraphicsRendition(GraphicsRendition::Blinking); break;
				case 7: _screen.setGraphicsRendition(GraphicsRendition::Inverse); break;
				case 8: _screen.setGraphicsRendition(GraphicsRendition::Hidden); break;
				case 9: _screen.setGraphicsRendition(GraphicsRendition::CrossedOut); break;
				case 21: _screen.setGraphicsRendition(GraphicsRendition::DoublyUnderlined); break;
				case 22: _screen.setGraphicsRendition(GraphicsRendition::Normal); break;
				case 23: _screen.setGraphicsRendition(GraphicsRendition::NoItalic); break;
				case 24: _screen.setGraphicsRendition(GraphicsRendition::NoUnderline); break;
				case 25: _screen.setGraphicsRendition(GraphicsRendition::NoBlinking); break;
				case 27: _screen.setGraphicsRendition(GraphicsRendition::NoInverse); break;
				case 28: _screen.setGraphicsRendition(GraphicsRendition::NoHidden); break;
				case 29: _screen.setGraphicsRendition(GraphicsRendition::NoCrossedOut); break;
				case 30: _screen.setForegroundColor(IndexedColor::Black); break;
				case 31: _screen.setForegroundColor(IndexedColor::Red); break;
				case 32: _screen.setForegroundColor(IndexedColor::Green); break;
				case 33: _screen.setForegroundColor(IndexedColor::Yellow); break;
				case 34: _screen.setForegroundColor(IndexedColor::Blue); break;
				case 35: _screen.setForegroundColor(IndexedColor::Magenta); break;
				case 36: _screen.setForegroundColor(IndexedColor::Cyan); break;
				case 37: _screen.setForegroundColor(IndexedColor::White); break;
				case 38: _screen.setForegroundColor(parseColor(_seq, &i)); break;
				case 39: _screen.setForegroundColor(DefaultColor{}); break;
				case 40: _screen.setBackgroundColor(IndexedColor::Black); break;
				case 41: _screen.setBackgroundColor(IndexedColor::Red); break;
				case 42: _screen.setBackgroundColor(IndexedColor::Green); break;
				case 43: _screen.setBackgroundColor(IndexedColor::Yellow); break;
				case 44: _screen.setBackgroundColor(IndexedColor::Blue); break;
				case 45: _screen.setBackgroundColor(IndexedColor::Magenta); break;
				case 46: _screen.setBackgroundColor(IndexedColor::Cyan); break;
				case 47: _screen.setBackgroundColor(IndexedColor::White); break;
				case 48: _screen.setBackgroundColor(parseColor(_seq, &i)); break;
				case 49: _screen.setBackgroundColor(DefaultColor{}); break;
                case 51: _screen.setGraphicsRendition(GraphicsRendition::Framed); break;
                case 53: _screen.setGraphicsRendition(GraphicsRendition::Overline); break;
                case 54: _screen.setGraphicsRendition(GraphicsRendition::NoFramed); break;
                case 55: _screen.setGraphicsRendition(GraphicsRendition::NoOverline); break;
                // 58 is reserved, but used for setting underline/decoration colors by some other VTEs (such as mintty, kitty, libvte)
                case 58: _screen.setUnderlineColor(parseColor(_seq, &i)); break;
				case 90: _screen.setForegroundColor(BrightColor::Black); break;
				case 91: _screen.setForegroundColor(BrightColor::Red); break;
				case 92: _screen.setForegroundColor(BrightColor::Green); break;
				case 93: _screen.setForegroundColor(BrightColor::Yellow); break;
				case 94: _screen.setForegroundColor(BrightColor::Blue); break;
				case 95: _screen.setForegroundColor(BrightColor::Magenta); break;
				case 96: _screen.setForegroundColor(BrightColor::Cyan); break;
				case 97: _screen.setForegroundColor(BrightColor::White); break;
				case 100: _screen.setBackgroundColor(BrightColor::Black); break;
				case 101: _screen.setBackgroundColor(BrightColor::Red); break;
				case 102: _screen.setBackgroundColor(BrightColor::Green); break;
				case 103: _screen.setBackgroundColor(BrightColor::Yellow); break;
				case 104: _screen.setBackgroundColor(BrightColor::Blue); break;
				case 105: _screen.setBackgroundColor(BrightColor::Magenta); break;
				case 106: _screen.setBackgroundColor(BrightColor::Cyan); break;
				case 107: _screen.setBackgroundColor(BrightColor::White); break;
				default: break; // TODO: logInvalidCSI("Invalid SGR number: {}", _seq.param(i));
			}
		}
		return ApplyResult::Ok;
	}

	ApplyResult requestMode(Sequence const& /*_seq*/, unsigned int _mode)
	{
		switch (_mode)
		{
			case 1: // GATM, Guarded area transfer
			case 2: // KAM, Keyboard action
			case 3: // CRM, Control representation
			case 4: // IRM, Insert/replace
			case 5: // SRTM, Status reporting transfer
			case 7: // VEM, Vertical editing
			case 10: // HEM, Horizontal editing
			case 11: // PUM, Positioning unit
			case 12: // SRM, Send/receive
			case 13: // FEAM, Format effector action
			case 14: // FETM, Format effector transfer
			case 15: // MATM, Multiple area transfer
			case 16: // TTM, Transfer termination
			case 17: // SATM, Selected area transfer
			case 18: // TSM, Tabulation stop
			case 19: // EBM, Editing boundary
			case 20: // LNM, Line feed/new line
				return ApplyResult::Unsupported; // TODO
			default:
				return ApplyResult::Invalid;
		}
	}

	ApplyResult requestModeDEC(Sequence const& /*_seq*/, unsigned int _mode)
	{
		switch (_mode)
		{
			case 1: // DECCKM, Cursor keys
			case 2: // DECANM, ANSI
			case 3: // DECCOLM, Column
			case 4: // DECSCLM, Scrolling
			case 5: // DECSCNM, Screen
			case 6: // DECOM, Origin
			case 7: // DECAWM, Autowrap
			case 8: // DECARM, Autorepeat
			case 18: // DECPFF, Print form feed
			case 19: // DECPEX, Printer extent
			case 25: // DECTCEM, Text cursor enable
			case 34: // DECRLM, Cursor direction, right to left
			case 35: // DECHEBM, Hebrew keyboard mapping
			case 36: // DECHEM, Hebrew encoding mode
			case 42: // DECNRCM, National replacement character set
			case 57: // DECNAKB, Greek keyboard mapping
			case 60: // DECHCCM*, Horizontal cursor coupling
			case 61: // DECVCCM, Vertical cursor coupling
			case 64: // DECPCCM, Page cursor coupling
			case 66: // DECNKM, Numeric keypad
			case 67: // DECBKM, Backarrow key
			case 68: // DECKBUM, Keyboard usage
			case 69: // DECVSSM / DECLRMM, Vertical split screen
			case 73: // DECXRLM, Transmit rate limiting
			case 81: // DECKPM, Key position
			case 95: // DECNCSM, No clearing screen on column change
			case 96: // DECRLCM, Cursor right to left
			case 97: // DECCRTSM, CRT save
			case 98: // DECARSM, Auto resize
			case 99: // DECMCM, Modem control
			case 100: // DECAAM, Auto answerback
			case 101: // DECCANSM, Conceal answerback message
			case 102: // DECNULM, Ignoring null
			case 103: // DECHDPXM, Half-duplex
			case 104: // DECESKM, Secondary keyboard language
			case 106: // DECOSCNM, Overscan
            case 2026: // Batched rendering (Synchronized output)
				return ApplyResult::Unsupported;
			default:
				return ApplyResult::Invalid;
		}
	}

    ApplyResult CPR(Sequence const& _seq, Screen& _screen)
    {
        switch (_seq.param(0))
        {
            case 5: _screen.deviceStatusReport(); return ApplyResult::Ok;
            case 6: _screen.reportCursorPosition(); return ApplyResult::Ok;
            default: return ApplyResult::Unsupported;
        }
    }

    ApplyResult DECRQPSR(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() != 1)
            return ApplyResult::Invalid; // -> error
        else if (_seq.param(0) == 1)
            // TODO: https://vt100.net/docs/vt510-rm/DECCIR.html
            // TODO return emitCommand<RequestCursorState>(); // or call it with ...Detailed?
            return ApplyResult::Invalid;
        else if (_seq.param(0) == 2)
        {
            _screen.requestTabStops();
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    ApplyResult DECSCUSR(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() <= 1)
        {
            switch (_seq.param_or(0, Sequence::Parameter{1}))
            {
                case 0:
                case 1: _screen.setCursorStyle(CursorDisplay::Blink, CursorShape::Block); break;
                case 2: _screen.setCursorStyle(CursorDisplay::Steady, CursorShape::Block); break;
                case 3: _screen.setCursorStyle(CursorDisplay::Blink, CursorShape::Underscore); break;
                case 4: _screen.setCursorStyle(CursorDisplay::Steady, CursorShape::Underscore); break;
                case 5: _screen.setCursorStyle(CursorDisplay::Blink, CursorShape::Bar); break;
                case 6: _screen.setCursorStyle(CursorDisplay::Steady, CursorShape::Bar); break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    ApplyResult ED(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() == 0)
            _screen.clearToEndOfScreen();
        else
        {
            for (size_t i = 0; i < _seq.parameterCount(); ++i)
            {
                switch (_seq.param(i))
                {
                    case 0: _screen.clearToEndOfScreen(); break;
                    case 1: _screen.clearToBeginOfScreen(); break;
                    case 2: _screen.clearScreen(); break;
                    case 3: _screen.clearScrollbackBuffer(); break;
                }
            }
        }
        return ApplyResult::Ok;
    }

    ApplyResult EL(Sequence const& _seq, Screen& _screen)
    {
        switch (_seq.param_or(0, Sequence::Parameter{0}))
        {
            case 0: _screen.clearToEndOfLine(); break;
            case 1: _screen.clearToBeginOfLine(); break;
            case 2: _screen.clearLine(); break;
            default: return ApplyResult::Invalid;
        }
        return ApplyResult::Ok;
    }

    ApplyResult TBC(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() != 1)
        {
            _screen.horizontalTabClear(HorizontalTabClear::AllTabs);
            return ApplyResult::Ok;
        }

        switch (_seq.param(0))
        {
            case 0: _screen.horizontalTabClear(HorizontalTabClear::UnderCursor); break;
            case 3: _screen.horizontalTabClear(HorizontalTabClear::AllTabs); break;
            default: return ApplyResult::Invalid;
        }
        return ApplyResult::Ok;
    }

    inline std::unordered_map<std::string_view, std::string_view> parseSubParamKeyValuePairs(std::string_view const& s)
    {
        return crispy::splitKeyValuePairs(s, ':');
    }

    ApplyResult setOrRequestDynamicColor(Sequence const& _seq, Screen& _screen, DynamicColorName _name)
    {
        auto const& value = _seq.intermediateCharacters();
        if (value == "?")
            _screen.requestDynamicColor(_name);
        else if (auto color = Sequencer::parseColor(value); color.has_value())
            _screen.setDynamicColor(_name, color.value());
        else
            return ApplyResult::Invalid;

        return ApplyResult::Ok;
    }

    ApplyResult clipboard(Sequence const& _seq, Screen& _screen)
    {
        // Only setting clipboard contents is supported, not reading.
        auto const& params = _seq.intermediateCharacters();
        if (auto const splits = crispy::split(params, ';'); splits.size() == 2 && splits[0] == "c")
        {
            _screen.eventListener().copyToClipboard(crispy::base64::decode(splits[1]));
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    ApplyResult NOTIFY(Sequence const& _seq, Screen& _screen)
    {
        auto const& value = _seq.intermediateCharacters();
        if (auto const splits = crispy::split(value, ';'); splits.size() == 3 && splits[0] == "notify")
        {
            _screen.notify(string(splits[1]), string(splits[2]));
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Unsupported;
    }

    ApplyResult HYPERLINK(Sequence const& _seq, Screen& _screen)
    {
        auto const& value = _seq.intermediateCharacters();
        // hyperlink_OSC ::= OSC '8' ';' params ';' URI
        // params := pair (':' pair)*
        // pair := TEXT '=' TEXT
        if (auto const pos = value.find(';'); pos != value.npos)
        {
            auto const paramsStr = value.substr(0, pos);
            auto const params = parseSubParamKeyValuePairs(paramsStr);

            auto id = string{};
            if (auto const p = params.find("id"); p != params.end())
                id = p->second;

            if (pos + 1 != value.size())
                _screen.hyperlink(id, value.substr(pos + 1));
            else
                _screen.hyperlink(string{id}, string{});

            return ApplyResult::Ok;
        }
        else
            _screen.hyperlink(string{}, string{});

        return ApplyResult::Ok;
    }

    ApplyResult DECRQSS(Sequence const& _seq, Screen& _screen)
    {
        auto const s = [](std::string const& _dataString) -> optional<RequestStatusString::Value> {
            auto const mappings = std::array<std::pair<std::string_view, RequestStatusString::Value>, 9>{
                pair{"m",   RequestStatusString::Value::SGR},
                pair{"\"p", RequestStatusString::Value::DECSCL},
                pair{" q",  RequestStatusString::Value::DECSCUSR},
                pair{"\"q", RequestStatusString::Value::DECSCA},
                pair{"r",   RequestStatusString::Value::DECSTBM},
                pair{"s",   RequestStatusString::Value::DECSLRM},
                pair{"t",   RequestStatusString::Value::DECSLPP},
                pair{"$|",  RequestStatusString::Value::DECSCPP},
                pair{"*|",  RequestStatusString::Value::DECSNLS}
            };
            for (auto const& mapping : mappings)
                if (_dataString == mapping.first)
                    return mapping.second;
            return nullopt;
        }(_seq.dataString());

        if (s.has_value())
        {
            _screen.requestStatusString(s.value());
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    ApplyResult saveDECModes(Sequence const& _seq, Screen& _screen)
    {
        vector<Mode> modes;
        for (size_t i = 0; i < _seq.parameterCount(); ++i)
            if (optional<Mode> mode = toDECMode(_seq.param(i)); mode.has_value())
                modes.push_back(mode.value());
        _screen.saveModes(modes);
        return ApplyResult::Ok;
    }

    ApplyResult restoreDECModes(Sequence const& _seq, Screen& _screen)
    {
        vector<Mode> modes;
        for (size_t i = 0; i < _seq.parameterCount(); ++i)
            if (optional<Mode> mode = toDECMode(_seq.param(i)); mode.has_value())
                modes.push_back(mode.value());
        _screen.restoreModes(modes);
        return ApplyResult::Ok;
    }

    ApplyResult WINDOWMANIP(Sequence const& _seq, Screen& _screen)
    {
        if (_seq.parameterCount() == 3)
        {
            switch (_seq.param(0))
            {
                case 4: _screen.eventListener().resizeWindow(_seq.param(2), _seq.param(1), true); break;
                case 8: _screen.eventListener().resizeWindow(_seq.param(2), _seq.param(1), false); break;
                case 22: _screen.saveWindowTitle(); break;
                case 23: _screen.restoreWindowTitle(); break;
                default: return ApplyResult::Unsupported;
            }
            return ApplyResult::Ok;
        }
        else if (_seq.parameterCount() == 1)
        {
            switch (_seq.param(0))
            {
                case 4: _screen.eventListener().resizeWindow(0, 0, true); break; // this means, resize to full display size
                case 8: _screen.eventListener().resizeWindow(0, 0, false); break; // i.e. full display size
                case 14: _screen.requestPixelSize(RequestPixelSize::Area::TextArea); break;
                default: return ApplyResult::Unsupported;
            }
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Unsupported;
    }

    ApplyResult XTSMGRAPHICS(Sequence const& _seq, Screen& _screen)
    {
        auto const Pi = _seq.param(0);
        auto const Pa = _seq.param(1);
        auto const Pv = _seq.param_or(2, 0);
        auto const Pu = _seq.param_or(3, 0);

        auto const item = [&]() -> optional<XtSmGraphics::Item> {
            switch (Pi) {
                case 1: return XtSmGraphics::Item::NumberOfColorRegisters;
                case 2: return XtSmGraphics::Item::SixelGraphicsGeometry;
                case 3: return XtSmGraphics::Item::ReGISGraphicsGeometry;
                default: return nullopt;
            }
        }();
        if (!item.has_value())
            return ApplyResult::Invalid;

        auto const action = [&]() -> optional<XtSmGraphics::Action> {
            switch (Pa) {
                case 1: return XtSmGraphics::Action::Read;
                case 2: return XtSmGraphics::Action::ResetToDefault;
                case 3: return XtSmGraphics::Action::SetToValue;
                case 4: return XtSmGraphics::Action::ReadLimit;
                default: return nullopt;
            }
        }();
        if (!action.has_value())
            return ApplyResult::Invalid;

        auto const value = [&]() -> XtSmGraphics::Value {
            using Action = XtSmGraphics::Action;
            switch (*action) {
                case Action::Read:
                case Action::ResetToDefault:
                case Action::ReadLimit:
                    return std::monostate{};
                case Action::SetToValue:
                    return *item == XtSmGraphics::Item::NumberOfColorRegisters
                        ? XtSmGraphics::Value{Pv}
                        : XtSmGraphics::Value{Size{Pv, Pu}};
            }
            return std::monostate{};
        }();

        _screen.smGraphics(*item, *action, value);

        return ApplyResult::Ok;
    }
} // }}}

// {{{ Sequence impl
std::string Sequence::raw() const
{
    stringstream sstr;

    switch (category_)
    {
        case FunctionCategory::C0: break;
        case FunctionCategory::ESC: sstr << "\033"; break;
        case FunctionCategory::CSI: sstr << "\033["; break;
        case FunctionCategory::DCS: sstr << "\033P"; break;
        case FunctionCategory::OSC: sstr << "\033]"; break;
    }

    if (parameterCount() > 1 || (parameterCount() == 1 && parameters_[0][0] != 0))
    {
        for (auto i = 0u; i < parameterCount(); ++i)
        {
            if (i)
                sstr << ';';

            sstr << param(i);
            for (auto k = 1u; k < subParameterCount(i); ++k)
                sstr << ':' << subparam(i, k);
        }
    }

    sstr << intermediateCharacters();

    if (finalChar_)
        sstr << finalChar_;

    if (!dataString_.empty())
        sstr << dataString_ << "\033\\";

    return sstr.str();
}

string Sequence::text() const
{
    stringstream sstr;

    sstr << fmt::format("{}", category_);

    if (leaderSymbol_)
        sstr << ' ' << leaderSymbol_;

    if (parameterCount() > 1 || (parameterCount() == 1 && parameters_[0][0] != 0))
    {
        sstr << ' ' << accumulate(
            begin(parameters_), end(parameters_), string{},
            [](string const& a, auto const& p) -> string {
                return !a.empty()
                    ? fmt::format("{};{}",
                            a,
                            accumulate(
                                begin(p), end(p),
                                string{},
                                [](string const& x, Sequence::Parameter y) -> string {
                                    return !x.empty()
                                        ? fmt::format("{}:{}", x, y)
                                        : std::to_string(y);
                                }
                            )
                        )
                    : accumulate(
                            begin(p), end(p),
                            string{},
                            [](string const& x, Sequence::Parameter y) -> string {
                                return !x.empty()
                                    ? fmt::format("{}:{}", x, y)
                                    : std::to_string(y);
                            }
                        );
            }
        );
    }

    if (!intermediateCharacters().empty())
        sstr << ' ' << intermediateCharacters();

    if (finalChar_)
        sstr << ' ' << finalChar_;

    if (!dataString_.empty())
        sstr << " \"" << crispy::escape(dataString_) << "\" ST";

    return sstr.str();
}
// }}}

Sequencer::Sequencer(Screen& _screen,
                     Logger _logger,
                     Size _maxImageSize,
                     RGBAColor _backgroundColor,
                     shared_ptr<ColorPalette> _imageColorPalette) :
    screen_{ _screen },
    logger_{ std::move(_logger) },
    imageColorPalette_{ std::move(_imageColorPalette) },
    maxImageSize_{ _maxImageSize },
    backgroundColor_{ _backgroundColor }
{
}

void Sequencer::error(std::string_view const& _errorString)
{
    logger_(ParserErrorEvent{string(_errorString)});
}

void Sequencer::print(char32_t _char)
{
    if (batching_)
        batchedSequences_.emplace_back(_char);
    else
    {
        instructionCounter_++;
        screen_.writeText(_char);
    }
}

void Sequencer::execute(char _controlCode)
{
    executeControlFunction(_controlCode);
}

void Sequencer::clear()
{
    sequence_.clear();
}

void Sequencer::collect(char _char)
{
    sequence_.intermediateCharacters().push_back(_char);
}

void Sequencer::collectLeader(char _leader)
{
    sequence_.setLeader(_leader);
}

void Sequencer::param(char _char)
{
    if (sequence_.parameters().empty())
        sequence_.parameters().push_back({0});

    switch (_char)
    {
        case ';':
            if (sequence_.parameters().size() < Sequence::MaxParameters)
                sequence_.parameters().push_back({0});
            break;
        case ':':
            if (sequence_.parameters().back().size() < Sequence::MaxParameters)
                sequence_.parameters().back().push_back({0});
            break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            sequence_.parameters().back().back() = sequence_.parameters().back().back() * 10 + (_char - U'0');
            break;
    }
}

void Sequencer::dispatchESC(char _finalChar)
{
    sequence_.setCategory(FunctionCategory::ESC);
    sequence_.setFinalChar(_finalChar);
    handleSequence();
}

void Sequencer::dispatchCSI(char _finalChar)
{
    sequence_.setCategory(FunctionCategory::CSI);
    sequence_.setFinalChar(_finalChar);
    handleSequence();
}

void Sequencer::startOSC()
{
    sequence_.setCategory(FunctionCategory::OSC);
}

void Sequencer::putOSC(char32_t _char)
{
    uint8_t u8[4];
    size_t const count = unicode::to_utf8(_char, u8);
    if (sequence_.intermediateCharacters().size() + count < Sequence::MaxOscLength)
        for (size_t i = 0; i < count; ++i)
            sequence_.intermediateCharacters().push_back(u8[i]);
}

void Sequencer::dispatchOSC()
{
    auto const [code, skipCount] = parseOSC(sequence_.intermediateCharacters());
    sequence_.parameters().push_back({static_cast<Sequence::Parameter>(code)});
    sequence_.intermediateCharacters().erase(0, skipCount);
    handleSequence();
    sequence_.clear();
}

void Sequencer::hook(char _finalChar)
{
    instructionCounter_++;
    sequence_.setCategory(FunctionCategory::DCS);
    sequence_.setFinalChar(_finalChar);
    if (FunctionDefinition const* funcSpec = sequence_.functionDefinition(); funcSpec != nullptr)
    {
        switch (funcSpec->id())
        {
            case DECSIXEL:
                hookedParser_ = hookSixel(sequence_);
                break;
            case DECRQSS:
                hookedParser_ = hookDECRQSS(sequence_);
                break;
#if defined(GOOD_IMAGE_PROTOCOL)
            case GIUPLOAD:
                hookedParser_ = hookGoodImageUpload(sequence_);
                break;
            case GIRENDER:
                hookedParser_ = hookGoodImageRender(sequence_);
                break;
            case GIDELETE:
                hookedParser_ = hookGoodImageRelease(sequence_);
                break;
            case GIONESHOT:
                hookedParser_ = hookGoodImageOneshot(sequence_);
                break;
#endif
        }

        if (hookedParser_)
            hookedParser_->start();
    }
}

void Sequencer::put(char32_t _char)
{
    if (hookedParser_)
        hookedParser_->pass(_char);
}

void Sequencer::unhook()
{
    if (hookedParser_)
    {
        hookedParser_->finalize();
        hookedParser_.reset();
    }
}

unique_ptr<ParserExtension> Sequencer::hookSixel(Sequence const& _seq)
{
    auto const Pa = _seq.param_or(0, 1);
    auto const Pb = _seq.param_or(1, 2);

    auto const aspectVertical = [](int Pa) {
        switch (Pa) {
            case 9:
            case 8:
            case 7:
                return 1;
            case 6:
            case 5:
                return 2;
            case 4:
            case 3:
                return 3;
            case 2:
                return 5;
            case 1:
            case 0:
            default:
                return 2;
        }
    }(Pa);

    auto const aspectHorizontal = 1;
    auto const transparentBackground = Pb != 1;

    sixelImageBuilder_ = make_unique<SixelImageBuilder>(
        maxImageSize_,
        aspectVertical,
        aspectHorizontal,
        transparentBackground
            ? RGBAColor{0, 0, 0, 0}
            : backgroundColor_,
        usePrivateColorRegisters_
            ? make_shared<ColorPalette>(maxImageRegisterCount_, min(maxImageRegisterCount_, 4096))
            : imageColorPalette_
    );

    return make_unique<SixelParser>(
        *sixelImageBuilder_,
        [this]() {
#if defined(CONTOUR_SYNCHRONIZED_OUTPUT)
            if (batching_)
            {
                batchedSequences_.emplace_back(SixelImage{
                    sixelImageBuilder_->size(),
                    sixelImageBuilder_->data()
                });
            }
            else
#endif
            {
                screen_.sixelImage(
                    sixelImageBuilder_->size(),
                    move(sixelImageBuilder_->data())
                );
            }
        }
    );
}

#if defined(GOOD_IMAGE_PROTOCOL) // {{{
namespace
{
    int toNumber(string const* _value, int _default)
    {
        if (!_value)
            return _default;

        int result = 0;
        for (char const ch : *_value)
        {
            if (ch >= '0' && ch <= '9')
                result = result * 10 + (ch - '0');
            else
                return _default;
        }

        return result;
    }

    optional<ImageAlignment> toImageAlignmentPolicy(string const* _value, ImageAlignment _default)
    {
        if (!_value)
            return _default;

        if (_value->size() != 1)
            return nullopt;

        switch (_value->at(0))
        {
            case '1': return ImageAlignment::TopStart;
            case '2': return ImageAlignment::TopCenter;
            case '3': return ImageAlignment::TopEnd;
            case '4': return ImageAlignment::MiddleStart;
            case '5': return ImageAlignment::MiddleCenter;
            case '6': return ImageAlignment::MiddleEnd;
            case '7': return ImageAlignment::BottomStart;
            case '8': return ImageAlignment::BottomCenter;
            case '9': return ImageAlignment::BottomEnd;
        }

        return nullopt;
    }

    optional<ImageResize> toImageResizePolicy(string const* _value, ImageResize _default)
    {
        if (!_value)
            return _default;

        if (_value->size() != 1)
            return nullopt;

        switch (_value->at(0))
        {
            case '0': return ImageResize::NoResize;
            case '1': return ImageResize::ResizeToFit;
            case '2': return ImageResize::ResizeToFill;
            case '3': return ImageResize::StretchToFill;
        }

        return nullopt; // TODO
    }

    optional<ImageFormat> toImageFormat(string const* _value)
    {
        auto constexpr DefaultFormat = ImageFormat::RGB;

        if (_value)
        {
            if (_value->size() == 1)
            {
                switch (_value->at(0))
                {
                    case '1': return ImageFormat::RGB;
                    case '2': return ImageFormat::RGBA;
                    case '3': return ImageFormat::PNG;
                    default: return nullopt;
                }
            }
            else
                return nullopt;
        }
        else
            return DefaultFormat;
    }
}

unique_ptr<ParserExtension> Sequencer::hookGoodImageUpload(Sequence const&)
{
    return make_unique<MessageParser>(
        [this](Message&& _message) {
            auto const name = _message.header("n");
            auto const imageFormat = toImageFormat(_message.header("f"));
            auto const width = toNumber(_message.header("w"), 0);
            auto const height = toNumber(_message.header("h"), 0);
            auto const size = Size{width, height};

            bool const validImage = imageFormat.has_value()
                                && ((*imageFormat == ImageFormat::PNG && !size.width && !size.height) ||
                                    (*imageFormat != ImageFormat::PNG && size.width && size.height));

            if (name && validImage)
            {
                screen_.uploadImage(*name, imageFormat.value(), size, _message.takeBody());
            }
        }
    );
}

unique_ptr<ParserExtension> Sequencer::hookGoodImageRender(Sequence const&)
{
    return make_unique<MessageParser>(
        [this](Message&& _message) {
            auto const screenRows = toNumber(_message.header("r"), 0);
            auto const screenCols = toNumber(_message.header("c"), 0);
            auto const name = _message.header("n");
            auto const x = toNumber(_message.header("x"), 0);           // XXX grid x offset
            auto const y = toNumber(_message.header("y"), 0);           // XXX grid y offset
            auto const imageWidth = toNumber(_message.header("w"), 0);  // XXX image width in grid coords
            auto const imageHeight = toNumber(_message.header("h"), 0); // XXX image height in grid coords
            auto const alignmentPolicy = toImageAlignmentPolicy(_message.header("a"), ImageAlignment::MiddleCenter);
            auto const resizePolicy = toImageResizePolicy(_message.header("z"), ImageResize::NoResize);
            auto const requestStatus = _message.header("s") != nullptr;
            auto const autoScroll = _message.header("l") != nullptr;

            auto const imageOffset = Coordinate{y, x};
            auto const imageSize = Size{imageWidth, imageHeight};
            auto const screenExtent = Size{screenCols, screenRows};

            screen_.renderImage(
                name ? *name : "",
                screenExtent,
                imageOffset,
                imageSize,
                *alignmentPolicy,
                *resizePolicy,
                autoScroll,
                requestStatus
            );
        }
    );
}

unique_ptr<ParserExtension> Sequencer::hookGoodImageRelease(Sequence const&)
{
    return make_unique<MessageParser>(
        [this](Message&& _message) {
            if (auto const name = _message.header("n"); name)
                screen_.releaseImage(*name);
        }
    );
}

unique_ptr<ParserExtension> Sequencer::hookGoodImageOneshot(Sequence const&)
{
    return make_unique<MessageParser>(
        [this](Message&& _message) {
            auto const imageFormat = toImageFormat(_message.header("f"));
            auto const imageWidth = toNumber(_message.header("w"), 0);
            auto const imageHeight = toNumber(_message.header("h"), 0);
            auto const screenRows = toNumber(_message.header("r"), 0);
            auto const screenCols = toNumber(_message.header("c"), 0);
            auto const alignmentPolicy = toImageAlignmentPolicy(_message.header("a"), ImageAlignment::MiddleCenter);
            auto const resizePolicy = toImageResizePolicy(_message.header("z"), ImageResize::NoResize);
            auto const autoScroll = _message.header("l") != nullptr;

            auto const imageSize = Size{imageWidth, imageHeight};
            auto const screenExtent = Size{screenCols, screenRows};

            screen_.renderImage(
                *imageFormat,
                imageSize ,
                _message.takeBody(),
                screenExtent,
                *alignmentPolicy,
                *resizePolicy,
                autoScroll
            );
        }
    );
}
#endif // }}}

unique_ptr<ParserExtension> Sequencer::hookDECRQSS(Sequence const& /*_seq*/)
{
    return make_unique<SimpleStringCollector>(
        [this](std::u32string const& _data) {
            auto const s = [](std::u32string const& _dataString) -> optional<RequestStatusString::Value> {
                auto const mappings = std::array<std::pair<std::u32string_view, RequestStatusString::Value>, 9>{
                    pair{U"m",   RequestStatusString::Value::SGR},
                    pair{U"\"p", RequestStatusString::Value::DECSCL},
                    pair{U" q",  RequestStatusString::Value::DECSCUSR},
                    pair{U"\"q", RequestStatusString::Value::DECSCA},
                    pair{U"r",   RequestStatusString::Value::DECSTBM},
                    pair{U"s",   RequestStatusString::Value::DECSLRM},
                    pair{U"t",   RequestStatusString::Value::DECSLPP},
                    pair{U"$|",  RequestStatusString::Value::DECSCPP},
                    pair{U"*|",  RequestStatusString::Value::DECSNLS}
                };
                for (auto const& mapping : mappings)
                    if (_dataString == mapping.first)
                        return mapping.second;
                return nullopt;
            }(_data);

            if (s.has_value())
                screen_.requestStatusString(s.value());

            // TODO: handle batching
        }
    );
}

void Sequencer::executeControlFunction(char _c0)
{
#if defined(CONTOUR_SYNCHRONIZED_OUTPUT)
    if (batching_)
    {
        sequence_.clear();
        sequence_.setCategory(FunctionCategory::C0);
        sequence_.setFinalChar(_c0);
        handleSequence();
        return;
    }
#endif

    instructionCounter_++;
    switch (_c0)
    {
        case 0x07: // BEL
            screen_.eventListener().bell();
            break;
        case 0x08: // BS
            screen_.backspace();
            break;
        case 0x09: // TAB
            screen_.moveCursorToNextTab();
            break;
        case 0x0A: // LF
            screen_.linefeed();
            break;
        case 0x0B: // VT
            // Even though VT means Vertical Tab, it seems that xterm is doing an IND instead.
            [[fallthrough]];
        case 0x0C: // FF
            // Even though FF means Form Feed, it seems that xterm is doing an IND instead.
            screen_.index();
            break;
        case 0x0D:
            screen_.moveCursorToBeginOfLine();
            break;
        case 0x37:
            screen_.saveCursor();
            break;
        case 0x38:
            screen_.restoreCursor();
            break;
        default:
            log<UnsupportedOutputEvent>(crispy::escape(_c0));
            break;
    }
}

void Sequencer::handleSequence()
{
#if defined(LIBTERMINAL_LOG_TRACE)
    logger_(TraceOutputEvent{fmt::format("{}", sequence_)});
#endif

    instructionCounter_++;
    if (FunctionDefinition const* funcSpec = sequence_.functionDefinition(); funcSpec != nullptr)
    {
#if defined(CONTOUR_SYNCHRONIZED_OUTPUT)
        if (*funcSpec == DECSM && sequence_.containsParameter(2026))
        {
            batching_ = true;
            apply(*funcSpec, sequence_);
        }
        else if (*funcSpec == DECRM && sequence_.containsParameter(2026))
        {
            batching_ = false;
            flushBatchedSequences();
            apply(*funcSpec, sequence_);
        }
        else if (batching_ && isBatchable(*funcSpec))
        {
            batchedSequences_.emplace_back(sequence_);
        }
        else
#endif
            apply(*funcSpec, sequence_);

        screen_.verifyState();
    }
    else
        std::cerr << fmt::format("Unknown VT sequence: {}\n", sequence_);
}

void Sequencer::flushBatchedSequences()
{
    for (auto const& batchable : batchedSequences_)
    {
        if (holds_alternative<char32_t>(batchable))
            print(get<char32_t>(batchable));
        else if (holds_alternative<Sequence>(batchable))
        {
            auto const& seq = get<Sequence>(batchable);
            if (FunctionDefinition const* spec = seq.functionDefinition(); spec != nullptr)
                apply(*spec, seq);
        }
        else if (holds_alternative<SixelImage>(batchable))
        {
            auto const& si = get<SixelImage>(batchable);
            screen_.sixelImage(si.size, Image::Data(si.rgba));
        }
    }
    batchedSequences_.clear();
}

/// Applies a FunctionDefinition to a given context, emitting the respective command.
ApplyResult Sequencer::apply(FunctionDefinition const& _function, Sequence const& _seq)
{
#if defined(CONTOUR_SYNCHRONIZED_OUTPUT)
    if (batching_ && isBatchable(_function))
    {
        batchedSequences_.emplace_back(_seq);
        return ApplyResult::Ok;
    }
#endif

    // This function assumed that the incoming instruction has been already resolved to a given
    // FunctionDefinition
    switch (_function)
    {
        // C0
        case BEL: screen_.eventListener().bell(); break;
        case BS: screen_.backspace(); break;
        case TAB: screen_.moveCursorToNextTab(); break;
        case LF: screen_.linefeed(); break;
        case VT: [[fallthrough]];
        case FF: screen_.index(); break;
        case CR: screen_.moveCursorToBeginOfLine(); break;

        // ESC
        case SCS_G0_SPECIAL: screen_.designateCharset(CharsetTable::G0, CharsetId::Special); break;
        case SCS_G0_USASCII: screen_.designateCharset(CharsetTable::G0, CharsetId::USASCII); break;
        case SCS_G1_SPECIAL: screen_.designateCharset(CharsetTable::G1, CharsetId::Special); break;
        case SCS_G1_USASCII: screen_.designateCharset(CharsetTable::G1, CharsetId::USASCII); break;
        case DECALN: screen_.screenAlignmentPattern(); break;
        case DECBI: screen_.backIndex(); break;
        case DECFI: screen_.forwardIndex(); break;
        case DECKPAM: screen_.applicationKeypadMode(true); break;
        case DECKPNM: screen_.applicationKeypadMode(false); break;
        case DECRS: screen_.restoreCursor(); break;
        case DECSC: screen_.saveCursor(); break;
        case HTS: screen_.horizontalTabSet(); break;
        case IND: screen_.index(); break;
        case NEL: screen_.moveCursorToNextLine(1); break;
        case RI: screen_.reverseIndex(); break;
        case RIS: screen_.resetHard(); break;
        case SS2: screen_.singleShiftSelect(CharsetTable::G2); break;
        case SS3: screen_.singleShiftSelect(CharsetTable::G3); break;

        // CSI
        case ANSISYSSC: screen_.restoreCursor(); break;
        case CBT: screen_.cursorBackwardTab(_seq.param_or(0, Sequence::Parameter{1})); break;
        case CHA: screen_.moveCursorToColumn(_seq.param_or(0, Sequence::Parameter{1})); break;
        case CHT: screen_.cursorForwardTab(_seq.param_or(0, Sequence::Parameter{1})); break;
        case CNL: screen_.moveCursorToNextLine(_seq.param_or(0, Sequence::Parameter{1})); break;
        case CPL: screen_.moveCursorToPrevLine(_seq.param_or(0, Sequence::Parameter{1})); break;
        case CPR: return impl::CPR(_seq, screen_);
        case CUB: screen_.moveCursorBackward(_seq.param_or(0, Sequence::Parameter{1})); break;
        case CUD: screen_.moveCursorDown(_seq.param_or(0, Sequence::Parameter{1})); break;
        case CUF: screen_.moveCursorForward(_seq.param_or(0, Sequence::Parameter{1})); break;
        case CUP: screen_.moveCursorTo(Coordinate{ _seq.param_or(0, 1), _seq.param_or(1, 1)}); break;
        case CUU: screen_.moveCursorUp(_seq.param_or(0, Sequence::Parameter{1})); break;
        case DA1: screen_.sendDeviceAttributes(); break;
        case DA2: screen_.sendTerminalId(); break;
        case DA3: return ApplyResult::Unsupported;
        case DCH: screen_.deleteCharacters(_seq.param_or(0, Sequence::Parameter{1})); break;
        case DECDC: screen_.deleteColumns(_seq.param_or(0, Sequence::Parameter{1})); break;
        case DECIC: screen_.insertColumns(_seq.param_or(0, Sequence::Parameter{1})); break;
        case DECRM:
            for_each(crispy::times(_seq.parameterCount()), [&](size_t i) {
                impl::setModeDEC(_seq, i, false, screen_);
            });
            break;
        case DECRQM: return impl::requestModeDEC(_seq, _seq.param(0));
        case DECRQM_ANSI: return impl::requestMode(_seq, _seq.param(0));
        case DECRQPSR: return impl::DECRQPSR(_seq, screen_);
        case DECSCUSR: return impl::DECSCUSR(_seq, screen_);
        case DECSCPP:
            if (auto const columnCount = _seq.param_or(0, 80); columnCount == 80 || columnCount == 132)
            {
                screen_.resizeColumns(columnCount, false);
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        case DECSLRM: screen_.setLeftRightMargin(_seq.param_opt(0), _seq.param_opt(1)); break;
        case DECSM: for_each(crispy::times(_seq.parameterCount()), [&](size_t i) { impl::setModeDEC(_seq, i, true, screen_); }); break;
        case DECSTBM: screen_.setTopBottomMargin(_seq.param_opt(0), _seq.param_opt(1)); break;
        case DECSTR: screen_.resetSoft(); break;
        case DECXCPR: screen_.reportExtendedCursorPosition(); break;
        case DL: screen_.deleteLines(_seq.param_or(0, Sequence::Parameter{1})); break;
        case ECH: screen_.eraseCharacters(_seq.param_or(0, Sequence::Parameter{1})); break;
        case ED: return impl::ED(_seq, screen_);
        case EL: return impl::EL(_seq, screen_);
        case HPA: screen_.moveCursorToColumn(_seq.param(0)); break;
        case HPR: screen_.moveCursorForward(_seq.param(0)); break;
        case HVP: screen_.moveCursorTo(Coordinate{_seq.param_or(0, Sequence::Parameter{1}), _seq.param_or(1, Sequence::Parameter{1})}); break; // YES, it's like a CUP!
        case ICH: screen_.insertCharacters(_seq.param_or(0, Sequence::Parameter{1})); break;
        case IL:  screen_.insertLines(_seq.param_or(0, Sequence::Parameter{1})); break;
        case RM:
            for_each(crispy::times(_seq.parameterCount()), [&](size_t i) {
                impl::setAnsiMode(_seq, i, false, screen_);
            });
            break;
        case SCOSC: screen_.saveCursor(); break;
        case SD: screen_.scrollDown(_seq.param_or(0, Sequence::Parameter{1})); break;
        case SETMARK: screen_.setMark(); break;
        case SGR: return impl::dispatchSGR(_seq, screen_);
        case SM: for_each(crispy::times(_seq.parameterCount()), [&](size_t i) { impl::setAnsiMode(_seq, i, true, screen_); }); break;
        case SU: screen_.scrollUp(_seq.param_or(0, Sequence::Parameter{1})); break;
        case TBC: return impl::TBC(_seq, screen_);
        case VPA: screen_.moveCursorToLine(_seq.param_or(0, Sequence::Parameter{1})); break;
        case WINMANIP: return impl::WINDOWMANIP(_seq, screen_);
        case DECMODERESTORE: return impl::restoreDECModes(_seq, screen_);
        case DECMODESAVE: return impl::saveDECModes(_seq, screen_);
        case XTSMGRAPHICS: return impl::XTSMGRAPHICS(_seq, screen_);

        // OSC
        case SETTITLE:
            //(not supported) ChangeIconTitle(_seq.intermediateCharacters());
            screen_.setWindowTitle(_seq.intermediateCharacters());
            return ApplyResult::Ok;
        case SETICON:
            //return emitCommand<ChangeIconTitle>(_output, _seq.intermediateCharacters());
            return ApplyResult::Unsupported;
        case SETWINTITLE: screen_.setWindowTitle(_seq.intermediateCharacters()); break;
        case SETXPROP: return ApplyResult::Unsupported;
        case HYPERLINK: return impl::HYPERLINK(_seq, screen_);
        case COLORFG: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::DefaultForegroundColor);
        case COLORBG: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::DefaultBackgroundColor);
        case COLORCURSOR: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::TextCursorColor);
        case COLORMOUSEFG: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::MouseForegroundColor);
        case COLORMOUSEBG: return impl::setOrRequestDynamicColor(_seq, screen_, DynamicColorName::MouseBackgroundColor);
        case CLIPBOARD: return impl::clipboard(_seq, screen_);
        // TODO: case COLORSPECIAL: return impl::setOrRequestDynamicColor(_seq, _output, DynamicColorName::HighlightForegroundColor);
        case RCOLORFG: screen_.resetDynamicColor(DynamicColorName::DefaultForegroundColor); break;
        case RCOLORBG: screen_.resetDynamicColor(DynamicColorName::DefaultBackgroundColor); break;
        case RCOLORCURSOR: screen_.resetDynamicColor(DynamicColorName::TextCursorColor); break;
        case RCOLORMOUSEFG: screen_.resetDynamicColor(DynamicColorName::MouseForegroundColor); break;
        case RCOLORMOUSEBG: screen_.resetDynamicColor(DynamicColorName::MouseBackgroundColor); break;
        case RCOLORHIGHLIGHTFG: screen_.resetDynamicColor(DynamicColorName::HighlightForegroundColor); break;
        case RCOLORHIGHLIGHTBG: screen_.resetDynamicColor(DynamicColorName::HighlightBackgroundColor); break;
        case NOTIFY: return impl::NOTIFY(_seq, screen_);
        case DUMPSTATE: screen_.dumpState(); break;
        default: return ApplyResult::Unsupported;
    }
    return ApplyResult::Ok;
}

std::optional<RGBColor> Sequencer::parseColor(std::string_view const& _value)
{
    try
    {
        // "rgb:RRRR/GGGG/BBBB"
        if (_value.size() == 18 && _value.substr(0, 4) == "rgb:" && _value[8] == '/' && _value[13] == '/')
        {
            auto const r = crispy::strntoul(_value.data() + 4, 4, nullptr, 16);
            auto const g = crispy::strntoul(_value.data() + 9, 4, nullptr, 16);
            auto const b = crispy::strntoul(_value.data() + 14, 4, nullptr, 16);

            return RGBColor{
                static_cast<uint8_t>(r & 0xFF),
                static_cast<uint8_t>(g & 0xFF),
                static_cast<uint8_t>(b & 0xFF)
            };
        }
        return std::nullopt;
    }
    catch (...)
    {
        // that will be a formatting error in stoul() then.
        return std::nullopt;
    }
}

std::string to_string(Mode _mode)
{
    switch (_mode)
    {
        case Mode::KeyboardAction: return "KeyboardAction";
        case Mode::Insert: return "Insert";
        case Mode::SendReceive: return "SendReceive";
        case Mode::AutomaticNewLine: return "AutomaticNewLine";
        case Mode::UseApplicationCursorKeys: return "UseApplicationCursorKeys";
        case Mode::DesignateCharsetUSASCII: return "DesignateCharsetUSASCII";
        case Mode::Columns132: return "Columns132";
        case Mode::SmoothScroll: return "SmoothScroll";
        case Mode::ReverseVideo: return "ReverseVideo";
        case Mode::MouseProtocolX10: return "MouseProtocolX10";
        case Mode::MouseProtocolNormalTracking: return "MouseProtocolNormalTracking";
        case Mode::MouseProtocolHighlightTracking: return "MouseProtocolHighlightTracking";
        case Mode::MouseProtocolButtonTracking: return "MouseProtocolButtonTracking";
        case Mode::MouseProtocolAnyEventTracking: return "MouseProtocolAnyEventTracking";
        case Mode::SaveCursor: return "SaveCursor";
        case Mode::ExtendedAltScreen: return "ExtendedAltScreen";
        case Mode::Origin: return "Origin";
        case Mode::AutoWrap: return "AutoWrap";
        case Mode::PrinterExtend: return "PrinterExtend";
        case Mode::LeftRightMargin: return "LeftRightMargin";
        case Mode::ShowToolbar: return "ShowToolbar";
        case Mode::BlinkingCursor: return "BlinkingCursor";
        case Mode::VisibleCursor: return "VisibleCursor";
        case Mode::ShowScrollbar: return "ShowScrollbar";
        case Mode::AllowColumns80to132: return "AllowColumns80to132";
        case Mode::UseAlternateScreen: return "UseAlternateScreen";
        case Mode::BracketedPaste: return "BracketedPaste";
        case Mode::FocusTracking: return "FocusTracking";
        case Mode::SixelScrolling: return "SixelScrolling";
        case Mode::UsePrivateColorRegisters: return "UsePrivateColorRegisters";
        case Mode::MouseExtended: return "MouseExtended";
        case Mode::MouseSGR: return "MouseSGR";
        case Mode::MouseURXVT: return "MouseURXVT";
        case Mode::MouseAlternateScroll: return "MouseAlternateScroll";
        case Mode::BatchedRendering: return "BatchedRendering";
    }
    return fmt::format("({})", static_cast<unsigned>(_mode));
};

// {{{ free function helpers
CursorShape makeCursorShape(string const& _name)
{
    string const name = [](string const& _input) {
        string output;
        transform(begin(_input), end(_input), back_inserter(output), [](auto ch) { return tolower(ch); });
        return output;
    }(_name);

    if (name == "block")
        return CursorShape::Block;
    else if (name == "rectangle")
        return CursorShape::Rectangle;
    else if (name == "underscore")
        return CursorShape::Underscore;
    else if (name == "bar")
        return CursorShape::Bar;
    else
        throw runtime_error{"Invalid cursor shape."};
}
// }}}

}  // namespace terminal

