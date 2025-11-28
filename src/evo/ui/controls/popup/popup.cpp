#include "../../../inc.hpp"

static int g_active_popup_id = -1;
static bool g_active_popup_state = false;
inline static auto str_index_dropdown = -1;
inline static bool state_dropdown = false;
inline static int time_dropdown;
static std::unordered_map<int, int> g_open_dropdown_index;

int elem_height[1000]{};

evo::popup_t::popup_t( int pop_id, std::string popup ) { 
	this->popup = popup;
	this->pop_id = pop_id;
	this->focused_element = false;
	this->hovered = false;
}

void evo::popup_t::handle_checkbox( ) {
	for ( int check = 0; check < this->checkbox_elems.size( ); check++ ) {
		auto animation = animation_controller.get( this->checkbox_elems[ check ].label + "#active" + std::to_string( this->pop_id ) + animation_controller.current_child );
		animation.adjust( animation.value + 3.f * animation_controller.get_min_deltatime( 0.4f ) * ( *this->checkbox_elems[ check ].value ? 1.f : -1.f ) );

		bool allow_interaction = this->focused_element;

		if ( allow_interaction &&
			evo::_input->mouse_in_box( evo::vec2_t( this->base_window.x + 15, this->base_window.y + 35 + elem_height[ this->pop_id ] ),
										evo::vec2_t( _container->group_width - 30, 30 ) ) && evo::_input->key_pressed( VK_LBUTTON ) ) {
			*this->checkbox_elems[ check ].value = !*this->checkbox_elems[ check ].value;
		}

		_ext->make_rect( this->base_window.x + 15, this->base_window.y + 35 + elem_height[ this->pop_id ], _container->group_width - 30,
						 30, _container->window_outline.modify_alpha( 80 * this->track_animation[ this->pop_id ] ), 2, 1 );

		_ext->make_rect_filled( this->base_window.x + 15 + _container->group_width - 60, this->base_window.y + 43 + elem_height[ this->pop_id ],
								15, 15, _container->window_backround.darker( 5 ).modify_alpha( 255 * this->track_animation[ this->pop_id ] )/*.blend( _container->window_accent, animation.value ).modify_alpha( 255 * this->track_animation[ _container->get_id( ) ] )*/, 2 );

		_ext->make_rect_filled( this->base_window.x + 15 + _container->group_width - 60, this->base_window.y + 43 + elem_height[ this->pop_id ],
								15 * animation.value, 15 * animation.value, _container->window_accent.darker( 5 ).modify_alpha( 255 * this->track_animation[ this->pop_id ] ), 2, true );

		_ext->make_rect_filled_shadow( this->base_window.x + 15 + _container->group_width - 60, this->base_window.y + 43 + elem_height[ this->pop_id ],
									   15, 15, _container->window_backround.darker( 5 ).blend( _container->window_accent, animation.value ).modify_alpha( 255 * this->track_animation[ this->pop_id ] ), 2, 15 );

		_ext->make_text( this->base_window.x + 25, this->base_window.y + 41 + elem_height[ this->pop_id ], _container->window_text.modify_alpha( 155 * this->track_animation[ this->pop_id ] ), evo::fonts_t::_default2,
						 this->checkbox_elems[ check ].label.c_str( ) );

		elem_height[this->pop_id ] += 35;
		_container->set_id( _container->get_id( ) + 1 );
	}
}

void evo::popup_t::handle_slider_int( ) {
	for ( auto& slider : this->elem_sliderint ) {
		_ext->make_rect( this->base_window.x + 15, this->base_window.y + 35 + elem_height[ this->pop_id ], _container->group_width - 30,
						 30, _container->window_outline.modify_alpha( 80 * this->track_animation[ this->pop_id ] ), 2, 1 );

		_ext->make_text( this->base_window.x + 25, this->base_window.y + 41 + elem_height[ this->pop_id ], _container->window_text.modify_alpha( 155 * this->track_animation[ this->pop_id ] ),
						 evo::fonts_t::_default2, slider.label.c_str( ), evo::font_flags_t::dropshadow_low );

		int _height{ 2 };
		int _added_height{};

		/* slider value */
		auto min_delta = *slider.value - slider.min;
		auto delta = static_cast< float >( slider.max - slider.min );
		auto total = ( min_delta / delta ) * ( _container->group_width - 30 );
		total = std::fmin( total, ( _container->group_width - 30 ) );

		if ( total > slider.min ) {
			_ext->make_rect_filled_shadow( this->base_window.x + 17, this->base_window.y + 56 + elem_height[ this->pop_id ] + 6, total - 2, 2, _container->window_accent.modify_alpha( 255 * this->track_animation[ this->pop_id ] ), 5, 10 );
			_ext->make_rect_filledex( this->base_window.x + 17, this->base_window.y + 56 + elem_height[ this->pop_id ] + 6, total - 2, 2, _container->window_accent.modify_alpha( 255 * this->track_animation[ this->pop_id ] ), 5, true );
		}

		/* input */
		{
			auto x = _container->group_width - 30;
			auto delta = slider.max - slider.min;
			static auto str_index = -1;
			bool allow_interaction = this->focused_element;

			int text_size = evo::_render->text_size( slider.label.c_str( ), evo::fonts_t::_default2 ).y;
			//this->hovered = evo::_input->mouse_in_box( vec2_t( this->base_window.x, this->base_window.y + text_size + 4 ), vec2_t( _container->group_width, 7 ) );

			if ( allow_interaction && _container->can_interact( ) && !theme::colorpicker_is_opened ) { // + 15 + elem_height
				if ( _input->key_pressed( VK_LBUTTON ) & 1 && evo::_input->mouse_in_box( vec2_t( this->base_window.x + 15, this->base_window.y + 35 + elem_height[ this->pop_id ] ), vec2_t( _container->group_width - 60, 30 ) ) ) {
					str_index = _container->get_id( );
				}
			}

			if ( allow_interaction && GetAsyncKeyState( VK_LBUTTON ) && str_index == _container->get_id( ) ) {
				//t//his->in_use = true;

				float normalized_pos = ( _input->get_mouse_position( ).x - this->base_window.x ) / ( _container->group_width - 30 );
				float target_value = slider.min + delta * normalized_pos;

				*slider.value = animation_controller.lerp_single( *slider.value, target_value, slider.max < 30 ? 1.f : 0.2f );

				//	std::cout << std::to_string( *slider.value ) + " v\n";

					/* gheto clamping */
				if ( *slider.value < slider.min )
					*slider.value = slider.min;

				if ( *slider.value > slider.max )
					*slider.value = slider.max;
			}

			/* update index */
			if ( !allow_interaction || !GetAsyncKeyState( VK_LBUTTON ) ) {
				//this->in_use = false;
				str_index = -1;
			}
		}

		std::string full_txt{};
		full_txt = std::to_string( *slider.value ) + slider.sufix;

		_ext->make_text( this->base_window.x + _container->group_width - 25 - _render->text_size( full_txt.c_str( ), 1 ).x, this->base_window.y + 41 + elem_height[ this->pop_id ], _container->window_text.modify_alpha( 155 * this->track_animation[ this->pop_id ] ),
								evo::fonts_t::_default2, full_txt.c_str( ) );

		elem_height[ this->pop_id ] += 35;
		_container->set_id( _container->get_id( ) + 1 );
	}

}

void evo::popup_t::handle_slider_float( ) { 
	for ( auto& slider : this->elem_sliderfloat ) {
		_ext->make_rect( this->base_window.x + 15, this->base_window.y + 35 + elem_height[ this->pop_id ], _container->group_width - 30,
						 30, _container->window_outline.modify_alpha( 80 * this->track_animation[ this->pop_id ] ), 2, 1 );

		_ext->make_text( this->base_window.x + 25, this->base_window.y + 41 + elem_height[ this->pop_id ], _container->window_text.modify_alpha( 155 * this->track_animation[ this->pop_id ] ),
						 evo::fonts_t::_default2, slider.label.c_str( ), evo::font_flags_t::dropshadow_low );

		int _height{ 2 };
		int _added_height{};

		/* slider value */
		auto min_delta = *slider.value - slider.min;
		auto delta = static_cast< float >( slider.max - slider.min );
		auto total = ( min_delta / delta ) * ( 200 - 30 );
		total = std::fmin( total, ( 200 - 30 ) );

		if ( total > slider.min ) {
			_ext->make_rect_filled_shadow( this->base_window.x + 17, this->base_window.y + 56 + elem_height[ this->pop_id ] + 6, total - 2, 2, _container->window_accent.modify_alpha( 255 * this->track_animation[ this->pop_id ] ), 5, 10 );
			_ext->make_rect_filledex( this->base_window.x + 17, this->base_window.y + 56 + elem_height[ this->pop_id ] + 6, total - 2, 2, _container->window_accent.modify_alpha( 255 * this->track_animation[ this->pop_id ] ), 5, true );
		}

		/* input */
		{
			auto x = _container->group_width - 30;
			auto delta = slider.max - slider.min;
			static auto str_index = -1;
			bool allow_interaction = this->focused_element;

			int text_size = evo::_render->text_size( slider.label.c_str( ), evo::fonts_t::_default2 ).y;
			//this->hovered = evo::_input->mouse_in_box( vec2_t( this->base_window.x, this->base_window.y + text_size + 4 ), vec2_t( _container->group_width, 7 ) );

			if ( allow_interaction && _container->can_interact( ) && !theme::colorpicker_is_opened ) { // + 15 + elem_height
				if ( _input->key_pressed( VK_LBUTTON ) & 1 && evo::_input->mouse_in_box( vec2_t( this->base_window.x + 15, this->base_window.y + 35 + elem_height[ this->pop_id ] ), vec2_t( _container->group_width - 60, 30 ) ) ) {
					str_index = _container->get_id( );
				}
			}

			if ( allow_interaction && GetAsyncKeyState( VK_LBUTTON ) && str_index == _container->get_id( ) ) {
				//t//his->in_use = true;

				float normalized_pos = ( _input->get_mouse_position( ).x - this->base_window.x ) / ( _container->group_width - 30 );
				float target_value = slider.min + delta * normalized_pos;

				*slider.value = animation_controller.lerp_single( *slider.value, target_value, slider.max < 30 ? 1.f : 0.2f );

				//	std::cout << std::to_string( *slider.value ) + " v\n";

					/* gheto clamping */
				if ( *slider.value < slider.min )
					*slider.value = slider.min;

				if ( *slider.value > slider.max )
					*slider.value = slider.max;
			}

			/* update index */
			if ( !allow_interaction || !GetAsyncKeyState( VK_LBUTTON ) ) {
				//this->in_use = false;
				str_index = -1;
			}
		}

		std::string full_txt{};
		full_txt = precision( *slider.value, 1 ) + slider.sufix;

		_ext->make_text( this->base_window.x + _container->group_width - 25 - _render->text_size( full_txt.c_str( ), 1 ).x, this->base_window.y + 41 + elem_height[ this->pop_id ], _container->window_text.modify_alpha( 155 * this->track_animation[ this->pop_id ] ),
						 evo::fonts_t::_default2, full_txt.c_str( ) );

		elem_height[ this->pop_id ] += 35;
		_container->set_id( _container->get_id( ) + 1 );
	}
}

void evo::popup_t::handle_text( ) { 
	for ( auto& text : this->elem_texct ) {
		_ext->make_rect( this->base_window.x + 15, this->base_window.y + 35 + elem_height[ this->pop_id ], _container->group_width - 30,
						 30, _container->window_outline.modify_alpha( 80 * this->track_animation[ this->pop_id ] ), 2, 1 );

		_ext->make_text( this->base_window.x + 15, this->base_window.y + 41 + elem_height[ this->pop_id ], _container->window_text.modify_alpha( 150 ), evo::fonts_t::_default2, text.label.c_str( ), evo::font_flags_t::dropshadow_low );
		
		elem_height[ this->pop_id ] += 35;
		_container->set_id( _container->get_id( ) + 1 );
	}
}

void evo::popup_t::handle_dropdown( ) {
	int open_dropdown_index = -1;
	if ( auto it = g_open_dropdown_index.find( this->pop_id ); it != g_open_dropdown_index.end( ) )
		open_dropdown_index = it->second;
	if ( !this->focused_element )
		open_dropdown_index = -1;

	for ( std::size_t dropdown_idx = 0; dropdown_idx < this->elem_drop.size( ); ++dropdown_idx ) {
		auto& dropdown = this->elem_drop[ dropdown_idx ];
		dropdown.focused = this->focused_element && ( open_dropdown_index == static_cast< int >( dropdown_idx ) );

		const float ctrl_width = _container->group_width - 30;
		const float ctrl_height = 30.f;
		const float base_x = this->base_window.x + 15;
		const float base_y = this->base_window.y + 35 + elem_height[ this->pop_id ];
		const float text_y = this->base_window.y + 41 + elem_height[ this->pop_id ];
		const float items_height = dropdown.focused ? static_cast<float>( dropdown.items.size( ) * 25 ) : 0.f;
		bool allow_interaction = this->focused_element;

		int current_value = dropdown.items.empty( ) ? 0 : std::clamp( *dropdown.value, 0, static_cast< int >( dropdown.items.size( ) ) - 1 );
		if ( !dropdown.items.empty( ) && current_value != *dropdown.value )
			*dropdown.value = current_value;

		bool header_hovered = evo::_input->mouse_in_box( { base_x, base_y }, { ctrl_width, ctrl_height } );
		bool list_hovered = false;
		if ( dropdown.focused && items_height > 0.f )
			list_hovered = evo::_input->mouse_in_box( { base_x, base_y + ctrl_height + 5.f }, { ctrl_width, items_height } );

		_ext->make_rect( base_x, base_y, ctrl_width, ctrl_height, _container->window_outline.modify_alpha( 80 * this->track_animation[ this->pop_id ] ), 2, 1 );
		_ext->make_text( base_x + 10, text_y, _container->window_text.modify_alpha( 155 * this->track_animation[ this->pop_id ] ), evo::fonts_t::_default2, dropdown.label.c_str( ) );

		std::string current_label = dropdown.items.empty( ) ? "none" : dropdown.items[ current_value ];
		auto text_size = _render->text_size( current_label.c_str( ), evo::fonts_t::_default2 );
		_ext->make_text( base_x + ctrl_width - 10 - text_size.x, text_y, _container->window_text.modify_alpha( 155 * this->track_animation[ this->pop_id ] ), evo::fonts_t::_default2, current_label.c_str( ) );
		_ext->make_text( base_x + ctrl_width - 20, text_y - 1, _container->window_text.modify_alpha( 155 * this->track_animation[ this->pop_id ] ), evo::fonts_t::_arrows, dropdown.focused ? "a" : "d" );

		if ( allow_interaction && _container->can_interact( ) && header_hovered && _input->key_pressed( VK_LBUTTON ) ) {
			dropdown.focused = !dropdown.focused;
			open_dropdown_index = dropdown.focused ? static_cast< int >( dropdown_idx ) : -1;
		}

		if ( dropdown.focused && allow_interaction && !header_hovered && !list_hovered && _input->key_pressed( VK_LBUTTON ) ) {
			dropdown.focused = false;
			if ( open_dropdown_index == static_cast< int >( dropdown_idx ) )
				open_dropdown_index = -1;
		}

		if ( dropdown.focused && !dropdown.items.empty( ) ) {
			float list_y = base_y + ctrl_height + 5.f;
			_ext->make_rect_filled( base_x, list_y, ctrl_width, items_height, _container->window_backround.modify_alpha( 255 * this->track_animation[ this->pop_id ] ), 2 );
			_ext->make_rect( base_x, list_y, ctrl_width, items_height, _container->window_outline.modify_alpha( 80 * this->track_animation[ this->pop_id ] ), 2, 1 );

			for ( std::size_t idx = 0; idx < dropdown.items.size( ); ++idx ) {
				float item_y = list_y + static_cast< float >( idx ) * 25.f;
				bool item_hovered = evo::_input->mouse_in_box( { base_x, item_y }, { ctrl_width, 25.f } );
				auto item_color = item_hovered ? _container->window_backround.darker( -5 ) : _container->window_backround;
				item_color = item_color.modify_alpha( 255 * this->track_animation[ this->pop_id ] );
				_ext->make_rect_filled( base_x, item_y, ctrl_width, 25.f, item_color, 0 );
				_ext->make_text( base_x + 10, item_y + 5, _container->window_text.modify_alpha( 200 * this->track_animation[ this->pop_id ] ), evo::fonts_t::_default2, dropdown.items[ idx ].c_str( ) );

				if ( allow_interaction && item_hovered && _input->key_pressed( VK_LBUTTON ) ) {
					*dropdown.value = static_cast< int >( idx );
					dropdown.focused = false;
					open_dropdown_index = -1;
					break;
				}
			}
		}

		int extra_height = dropdown.focused ? static_cast< int >( items_height ) + 5 : 0;
		elem_height[ this->pop_id ] += 35 + extra_height;
		_container->set_id( _container->get_id( ) + 1 );
	}

	if ( open_dropdown_index >= 0 )
		g_open_dropdown_index[ this->pop_id ] = open_dropdown_index;
	else
		g_open_dropdown_index.erase( this->pop_id );
}

void evo::popup_t::paint( ) { 
	elem_height[ this->pop_id ] = 0;

	auto anim_2 = animation_controller.get( std::to_string( _container->get_id( ) ) + "#color22picker_focused" + animation_controller.current_child );
	anim_2.adjust( anim_2.value + 3.f * animation_controller.get_min_deltatime( 0.5f ) * ( ( _container->anim_controler > 0.f ) && this->focused_element ? 1.f : -1.f ) );


	this->track_animation[ this->pop_id ] = anim_2.value;

	_render->add_rect_filled( this->base_window.x, this->base_window.y, _container->group_width,
							  30, _container->window_backround.modify_alpha( 255 * _container->anim_controler ).blend(
								  _container->window_backround.darker( -5 ), anim_2.value
							  ), 2 );

	this->handle_checkbox( );
	this->handle_slider_int( );
	this->handle_slider_float( );
	this->handle_dropdown( );
	this->handle_text( );
	
	// store.
	this->track_size[ this->pop_id ] = elem_height[ this->pop_id ];

	// element and stuff
	_render->add_rect_filled( this->base_window.x, this->base_window.y + 30, _container->group_width,
							  5 + elem_height[ this->pop_id ] * anim_2.value, _container->window_backround.modify_alpha( 255 * anim_2.value ), 2 );

	_render->add_rect_filled( this->base_window.x, this->base_window.y + 30, _container->group_width,
		1, _container->window_outline.modify_alpha( 255 * anim_2.value ), 2 );

	// other shit 
	_render->add_rect( this->base_window.x, this->base_window.y, _container->group_width,
					   30 + ( elem_height[ this->pop_id ] + 5) *anim_2.value, _container->window_outline.modify_alpha( 80 * _container->anim_controler ), 2, 1 );

	_render->add_text( this->base_window.x + 10, this->base_window.y + 5,
					   _container->window_text.modify_alpha( 155 * _container->anim_controler ), 1, this->popup.c_str( ), 0 );

	_render->add_text( this->base_window.x + _container->group_width - 30, this->base_window.y + 6,
					   _container->window_text.modify_alpha( 155 * _container->anim_controler ), evo::fonts_t::_arrows, this->focused_element ? "a" : "d", 0 );


}

void evo::popup_t::input( ) { 
	bool header_hovered = _input->mouse_in_box( { this->base_window.x, this->base_window.y }, { ( float )_container->group_width, 30 } );
	bool body_hovered = _input->mouse_in_box( { this->base_window.x, this->base_window.y }, { ( float )_container->group_width, 30 + ( float )elem_height[ this->pop_id ] } );

	if ( _container->can_interact( ) && header_hovered && _input->key_pressed( VK_LBUTTON ) && !_container->base_handler[ 3 ] ) {
		if ( g_active_popup_id == _container->get_id( ) ) {
			g_active_popup_state = !g_active_popup_state;
			if ( !g_active_popup_state )
				g_active_popup_id = -1;
		} else {
			g_active_popup_id = _container->get_id( );
			g_active_popup_state = true;
		}
	}

	if ( g_active_popup_id == _container->get_id( ) )
		this->focused_element = g_active_popup_state;
	else
		this->focused_element = false;

	if ( g_active_popup_id == _container->get_id( ) && !body_hovered && _input->key_pressed( VK_LBUTTON ) ) {
		g_active_popup_state = false;
		g_active_popup_id = -1;
		this->focused_element = false;
	}

	_container->base_handler[ 0 ] = g_active_popup_state;
	_container->base_opened_state[ _container->base_handler_t::colorpicker ][ _container->get_id( ) ] = this->focused_element;
	this->hovered = body_hovered;
}

void evo::popup_t::bind_checkbox( std::string label, bool* value ) {
	this->checkbox_elems.push_back( elem_check( label, value ) );
}

void evo::popup_t::bind_slider_int( std::string label, int* value, int min, int max, std::string sufix ) { 
	this->elem_sliderint.push_back( elem_slider_int( label, value, min, max, sufix ) );
}

void evo::popup_t::bind_slider_float( std::string label, float* value, int min, int max, std::string sufix ) {
	this->elem_sliderfloat.push_back( elem_slider_float( label, value, min, max, sufix ) );
}

void evo::popup_t::bind_text( std::string label ) { 
	this->elem_texct.push_back( elem_text( label ) );
}

void evo::popup_t::bind_dropdown( std::string label, int* value, std::vector<std::string> items ) { 
	this->elem_drop.push_back( elem_dropdown( label, value, items ) );
}

void evo::reset_all_popups()
{
	g_active_popup_state = false;
	g_active_popup_id = -1;
	g_open_dropdown_index.clear();
	for (auto &height : elem_height)
		height = 0;
}
