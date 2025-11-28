#pragma once

namespace evo {
	class text_t {
	public:
		text_t( std::string label, float min_height = 35.f );
	public:
		void paint( );
		float get_consumed_height( ) const;

		vec2_t base_window{};
	private:
		std::string label{};
		float min_height{ 35.f };
		float consumed_height{ 35.f };
	};
}