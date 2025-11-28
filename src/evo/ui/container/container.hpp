#pragma once
#include "../controls/esp_preview/esp_preview.hpp"
namespace evo {
	/* theme and shit */
	namespace theme {
		/* ints */
		inline int selected_tab{}, selected_subtab{ }, orx{ 1020 }, ory{ 660 }, rx{ 1020 }, ry{ 660 };

		inline bool dragging{}, resizing{};
		inline bool colorpicker_is_opened{};

		inline int cond_id[ 64 ]{};

		inline std::vector< movable_item > item_data{};

		inline col_t copied_color{};
		inline std::string footer_main { "Roblox" };
		inline std::string footer_suffix { " Developer Build" };

		/* vec2 */
		inline vec2_t menu_spawn{ 100, 100 }, menu_size{ 1020, 660 };
		inline bool menu_resizable{ false };
		inline bool menu_open{ false };
	}

	/* menu container */
	class container_t {
	public: /* container stuff */
		/* colors */
		evo::col_t window_backround{}, window_outline{}, window_title_bar{}, window_title_bar_second{}, window_outline_second{}, window_text{}, window_accent{},
			child_backround{}, elem_back{};

		int base_element_id{};
		int group_width{};
	public: /* init container */
		void initialize_container( );
	public: /* id handling */
		enum base_handler_t {
			combo,
			multicombo,
			colorpicker,
			textbox,
			bind,
			child_dropped,
			max_size
		};

		bool base_handler[ 5 ]{};
		bool base_opened_state[ base_handler_t::max_size ][ 1000 ];

		int base_opened[ base_handler_t::max_size ];
	public: /* id handling - part 2 */
		int get_id( );
		bool can_interact( );

		void set_id( const uintptr_t last );
	public: /* animation externals */
		float anim_controler{};
	};

	inline const auto _container = std::make_unique< container_t >( );
}