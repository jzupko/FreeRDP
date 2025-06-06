/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client helper dialogs
 *
 * Copyright 2023 Armin Novak <armin.novak@thincast.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cassert>

#include <string>
#include <utility>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "sdl_select.hpp"
#include "sdl_widget.hpp"
#include "sdl_button.hpp"
#include "sdl_buttons.hpp"
#include "sdl_input_widget_pair_list.hpp"

SdlSelectWidget::SdlSelectWidget(std::shared_ptr<SDL_Renderer>& renderer, const std::string& label,
                                 const SDL_FRect& rect)
    : SdlSelectableWidget(renderer, rect)
{
	_backgroundcolor = { 0x69, 0x66, 0x63, 0xff };
	_fontcolor = { 0xd1, 0xcf, 0xcd, 0xff };
	update_text(label);
}

SdlSelectWidget::~SdlSelectWidget() = default;

SdlSelectWidget::SdlSelectWidget(SdlSelectWidget&& other) noexcept = default;
