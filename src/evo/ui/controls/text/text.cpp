#include "../../../inc.hpp"

namespace {
	static std::vector<std::string> split_lines( const std::string& text ) {
		std::vector<std::string> lines;
		std::string current;
		current.reserve( text.size( ) + 1 );

		for ( const char ch : text ) {
			if ( ch == '\r' ) {
				continue;
			}

			if ( ch == '\n' ) {
				lines.push_back( current );
				current.clear( );
			} else {
				current.push_back( ch );
			}
		}

		lines.push_back( current );
		return lines;
	}
}

evo::text_t::text_t( std::string label, float min_height ) {
	this->label = label;
	this->min_height = min_height;
}

void evo::text_t::paint( ) {
	constexpr float kBaseHeight = 30.f;
	constexpr float kLineHeight = 18.f;

	auto lines = split_lines( this->label );
	if ( lines.empty( ) ) {
		lines.emplace_back( "" );
	}

	const float extra_height = lines.size( ) > 1 ? ( lines.size( ) - 1 ) * kLineHeight : 0.f;
	float total_height = kBaseHeight + extra_height;
	if ( total_height < this->min_height ) {
		total_height = this->min_height;
	}
	this->consumed_height = total_height + 5.f;

	_render->add_rect_filled( this->base_window.x, this->base_window.y, _container->group_width,
							  total_height, _container->window_backround.modify_alpha( 255 * _container->anim_controler ), 2 );

	_render->add_rect( this->base_window.x, this->base_window.y, _container->group_width,
				   total_height, _container->window_outline.modify_alpha( 80 * _container->anim_controler ), 2, 1 );

	_render->add_rect_filled( this->base_window.x, this->base_window.y + kBaseHeight, _container->group_width,
							  1, _container->window_outline.modify_alpha( 255 * _container->anim_controler ), 2 );

	for ( std::size_t i = 0; i < lines.size( ); ++i ) {
		const float line_y = this->base_window.y + 5.f + static_cast<float>( i ) * kLineHeight;
		evo::_render->add_text( this->base_window.x + 10, line_y, _container->window_text.modify_alpha( 155 * _container->anim_controler ), evo::fonts_t::_default2,
							lines[ i ].c_str( ) );
	}
}

float evo::text_t::get_consumed_height( ) const {
	return this->consumed_height;
}
