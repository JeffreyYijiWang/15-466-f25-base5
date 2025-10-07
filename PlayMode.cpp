#include "PlayMode.hpp"

#include "DrawLines.hpp"
#include "gl_errors.hpp"
#include "data_path.hpp"
#include "hex_dump.hpp"

#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>

#include <random>
#include <array>

PlayMode::PlayMode(Client &client_) : client(client_) {

	//text @GPT
	text = std::make_unique<TextHB>();
    bool ok = text->init(data_path("Delius-Regular.ttf"), 32);
    assert(ok && "Failed to init TextHB");

    std::string err;
    ok = dialog.load_from_file(data_path("dialogues.txt"), &err);
    assert(ok && "Failed to load dialogues.txt");
    if(!ok) SDL_Log("dialog load error: %s", err.c_str());

    cur_state = dialog.start_id;
    selected = 0;
    finished = false;

	set_theme(theme_id);
}

PlayMode::~PlayMode() {
}



void PlayMode::set_theme(ThemeId id) {
	theme_id = id;
	switch (id) {
	case ThemeId::Light:
		theme.bg   = {0.95f, 0.96f, 0.98f};
		theme.grid = {0.40f, 0.45f, 0.60f};
		theme.red  = {0.83f, 0.20f, 0.22f};
		theme.blue = {0.16f, 0.35f, 0.86f};
		theme.allyGlow = {0.25f, 0.25f, 0.0f};
		theme.panelBG  = {0.90f, 0.92f, 0.96f};
		theme.textDim  = {0.15f, 0.18f, 0.24f};
		theme.textBright = {0.05f, 0.07f, 0.10f};
		break;
	case ThemeId::Neo:
		theme = Theme{}; // defaults declared in header
		break;
	case ThemeId::Terminal:
		theme.bg   = {0.02f, 0.05f, 0.02f};
		theme.grid = {0.00f, 0.60f, 0.00f};
		theme.red  = {0.00f, 1.00f, 0.33f};
		theme.blue = {0.33f, 1.00f, 0.66f};
		theme.allyGlow = {0.80f, 1.00f, 0.80f};
		theme.panelBG  = {0.00f, 0.10f, 0.00f};
		theme.textDim  = {0.40f, 1.00f, 0.40f};
		theme.textBright = {0.80f, 1.00f, 0.80f};
		break;
	}
}


bool PlayMode::handle_event(SDL_Event const &evt, glm::uvec2 const &window_size) {

//QUICK THEME CYCLE
	if (evt.type == SDL_EVENT_KEY_DOWN && !evt.key.repeat) {
		if      (evt.key.key == SDLK_F1) set_theme(ThemeId::Light);
		else if (evt.key.key == SDLK_F2) set_theme(ThemeId::Neo);
		else if (evt.key.key == SDLK_F3) set_theme(ThemeId::Terminal);
	}

	if (evt.type == SDL_EVENT_KEY_DOWN) {
		if (evt.key.repeat) {
			//ignore repeats
		} else if (evt.key.key == SDLK_A) {
			controls.left.downs += 1;
			controls.left.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_D) {
			controls.right.downs += 1;
			controls.right.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_W) {
			controls.up.downs += 1;
			controls.up.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_S) {
			controls.down.downs += 1;
			controls.down.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_SPACE) {
			controls.jump.downs += 1;
			controls.jump.pressed = true;
			return true;
		}
	} else if (evt.type == SDL_EVENT_KEY_UP) {
		if (evt.key.key == SDLK_A) {
			controls.left.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_D) {
			controls.right.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_W) {
			controls.up.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_S) {
			controls.down.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_SPACE) {
			controls.jump.pressed = false;
			return true;
		}
	}

	return false;
}

void PlayMode::update(float elapsed) {

	//queue data for sending to server:
	controls.send_controls_message(&client.connection);

	//reset button press counters:
	controls.left.downs = 0;
	controls.right.downs = 0;
	controls.up.downs = 0;
	controls.down.downs = 0;
	controls.jump.downs = 0;

	//send/receive data:
	client.poll([this](Connection *c, Connection::Event event){
		if (event == Connection::OnOpen) {
			std::cout << "[" << c->socket << "] opened" << std::endl;
		} else if (event == Connection::OnClose) {
			std::cout << "[" << c->socket << "] closed (!)" << std::endl;
			throw std::runtime_error("Lost connection to server!");
		} else { assert(event == Connection::OnRecv);
			//std::cout << "[" << c->socket << "] recv'd data. Current buffer:\n" << hex_dump(c->recv_buffer); std::cout.flush(); //DEBUG
			bool handled_message;
			try {
				do {
					handled_message = false;
					if (game.recv_state_message(c)) handled_message = true;
				} while (handled_message);
			} catch (std::exception const &e) {
				std::cerr << "[" << c->socket << "] malformed message from server: " << e.what() << std::endl;
				//quit the game:
				throw e;
			}
		}
	}, 0.0);

	if (!game.players.empty()) {
		auto &me = game.players.front();
		
		inline Team infer_team_from_color(const glm::vec3 &rgb) {
			if (rgb.b > rgb.r) return Team::Blue;
			return Team::Red;
		}
		my_team = infer_team_from_color(me.color);
	}
}


glm::u8vec4 PlayMode::color_for_player(glm::vec3 base, bool is_ally) const {
	glm::vec3 c = base;
	if (is_ally) {
		// subtle glow towards allyGlow
		c = glm::mix(c, theme.allyGlow, 0.25f);
	}
	return glm::u8vec4(
		glm::clamp(int(c.r * 255.0f), 0, 255),
		glm::clamp(int(c.g * 255.0f), 0, 255),
		glm::clamp(int(c.b * 255.0f), 0, 255),
		255);
}

void PlayMode::draw_sidebar(glm::uvec2 const &drawable_size) {
	// Left panel background:
	glDisable(GL_DEPTH_TEST);
	glViewport(0, 0, drawable_size.x, drawable_size.y);
	glClear(GL_DEPTH_BUFFER_BIT); // keep color buffer

	text->begin(drawable_size);

	const float panel_w = 280.0f;
	const float x0 = 24.0f;
	float y = 28.0f;
	const float line_h = 28.0f;

	auto draw_label = [&](const std::string& s, glm::vec3 col){
		text->draw_text(s, x0, y, col);
		y += line_h;
	};

	// Collect names by team:
	std::vector<std::string> red_names, blue_names;
	std::unordered_set<std::string> my_names;
	collect_members(red_names, blue_names, my_names);

	// Header:
	draw_label("=== Teams ===", theme.textBright);

	// Scores:
	{
		// Scores (no ostringstream):
		draw_label(std::string("Red  : ") + std::to_string(score_red), theme.red);
		draw_label(std::string("Blue : ") + std::to_string(score_blue), theme.blue);
		y += 8.0f;
	}

	// Current turn (if known):
	if (current_team.has_value()) {
		std::string who = (*current_team == Team::Red) ? "Red" : "Blue";
		draw_label("Turn : " + who, theme.textBright);
		y += 8.0f;
	}

	// Member lists:
	draw_label("Red Team:", theme.red);
	for (auto &n : red_names) {
		bool mine = my_names.count(n) != 0;
		text->draw_text((mine ? "▶ " : "  ") + n, x0 + 8.0f, y, mine ? theme.textBright : theme.textDim);
		y += line_h;
	}
	y += 8.0f;
	draw_label("Blue Team:", theme.blue);
	for (auto &n : blue_names) {
		bool mine = my_names.count(n) != 0;
		text->draw_text((mine ? "▶ " : "  ") + n, x0 + 8.0f, y, mine ? theme.textBright : theme.textDim);
		y += line_h;
	}

	text->end();
}

void PlayMode::collect_members(std::vector<std::string>& red_names,
                               std::vector<std::string>& blue_names,
                               std::unordered_set<std::string>& my_team_names) const {
	red_names.clear(); blue_names.clear(); my_team_names.clear();
	std::optional<Team> mine = my_team;

	for (auto const &p : game.players) {
		auto t = infer_team_from_color(p.color);
		if (t == Team::Red) red_names.push_back(p.name);
		else blue_names.push_back(p.name);
		if (mine.has_value() && t == *mine) my_team_names.insert(p.name);
	}
	std::sort(red_names.begin(), red_names.end());
	std::sort(blue_names.begin(), blue_names.end());
}

void PlayMode::draw_arena_and_players(glm::uvec2 const &drawable_size) {
	// camera-to-world
	float aspect = float(drawable_size.x) / float(drawable_size.y);
	float scale = std::min(
		2.0f * aspect / (Game::ArenaMax.x - Game::ArenaMin.x + 2.0f * Game::PlayerRadius),
		2.0f / (Game::ArenaMax.y - Game::ArenaMin.y + 2.0f * Game::PlayerRadius)
	);
	glm::vec2 offset = -0.5f * (Game::ArenaMax + Game::ArenaMin);

	glm::mat4 world_to_clip = glm::mat4(
		scale / aspect, 0.0f, 0.0f, offset.x,
		0.0f, scale, 0.0f, offset.y,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	);

	// lines helper
	DrawLines lines(world_to_clip);

	// Arena border
	glm::u8vec4 gridc = glm::u8vec4(theme.grid.r*255, theme.grid.g*255, theme.grid.b*255, 255);
	lines.draw(glm::vec3(Game::ArenaMin.x, Game::ArenaMin.y, 0.0f), glm::vec3(Game::ArenaMax.x, Game::ArenaMin.y, 0.0f), gridc);
	lines.draw(glm::vec3(Game::ArenaMin.x, Game::ArenaMax.y, 0.0f), glm::vec3(Game::ArenaMax.x, Game::ArenaMax.y, 0.0f), gridc);
	lines.draw(glm::vec3(Game::ArenaMin.x, Game::ArenaMin.y, 0.0f), glm::vec3(Game::ArenaMin.x, Game::ArenaMax.y, 0.0f), gridc);
	lines.draw(glm::vec3(Game::ArenaMax.x, Game::ArenaMin.y, 0.0f), glm::vec3(Game::ArenaMax.x, Game::ArenaMax.y, 0.0f), gridc);

	// draw players
	std::optional<Team> mine = my_team;
	for (auto const &player : game.players) {
		bool ally = (mine.has_value() && infer_team_from_color(player.color) == *mine);

		glm::u8vec4 col = color_for_player(player.color, ally);

		// mark first player (you) with an 'X':
		if (&player == &game.players.front()) {
			lines.draw(
				glm::vec3(player.position + Game::PlayerRadius * glm::vec2(-0.5f,-0.5f), 0.0f),
				glm::vec3(player.position + Game::PlayerRadius * glm::vec2( 0.5f, 0.5f), 0.0f), col);
			lines.draw(
				glm::vec3(player.position + Game::PlayerRadius * glm::vec2(-0.5f, 0.5f), 0.0f),
				glm::vec3(player.position + Game::PlayerRadius * glm::vec2( 0.5f,-0.5f), 0.0f), col);
		}

		for (uint32_t a = 0; a < circle_pts.size(); ++a) {
			lines.draw(
				glm::vec3(player.position + Game::PlayerRadius * circle_pts[a], 0.0f),
				glm::vec3(player.position + Game::PlayerRadius * circle_pts[(a+1)%circle_pts.size()], 0.0f), col);
		}

		// name under disc:
		auto draw_label = [&](glm::vec2 at, std::string const &lab, float H) {
			lines.draw_text(lab,
				glm::vec3(at.x, at.y, 0.0),
				glm::vec3(H, 0.0f, 0.0f), glm::vec3(0.0f, H, 0.0f),
				glm::u8vec4(0x00,0x00,0x00,0x00));
			float ofs = (1.0f / scale) / drawable_size.y;
			lines.draw_text(lab,
				glm::vec3(at.x + ofs, at.y + ofs, 0.0),
				glm::vec3(H, 0.0f, 0.0f), glm::vec3(0.0f, H, 0.0f),
				glm::u8vec4(0xff,0xff,0xff,0x00));
		};
		draw_label(player.position + glm::vec2(0.0f, -0.1f + Game::PlayerRadius), player.name, 0.09f);
	}
}
void PlayMode::draw(glm::uvec2 const &drawable_size) {

	// https://github.com/jialand/TheMuteLift/blob/main/PlayMode.cpp
	//reference form JialanD/TheMuteLift and chatgpt
	static std::array< glm::vec2, 16 > const circle = [](){
		std::array< glm::vec2, 16 > ret;
		for (uint32_t a = 0; a < ret.size(); ++a) {
			float ang = a / float(ret.size()) * 2.0f * float(M_PI);
			ret[a] = glm::vec2(std::cos(ang), std::sin(ang));
		}
		return ret;
	}();

	glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);

    // draw text：@GPT referenced
    text->begin(drawable_size);

    const float margin_l = 64.0f;
    const float margin_r = 64.0f;
    const float start_x  = margin_l;
    const float start_y  = 100.0f;   // baseline of first line
    const float line_h   = 42.0f;
    const float opt_gap  = 22.0f;

    float max_width = float(drawable_size.x) - margin_l - margin_r;

    const DialogueNode* node = dialog.get(cur_state);
    if (node) {
        // --- body with auto wrap ---
        std::vector<std::string> body_lines;
        text->wrap_text(node->text, max_width, body_lines);

        float y = start_y;
        for (auto const& ln : body_lines) {
            if (!ln.empty())
                text->draw_text(ln, start_x, y, glm::vec3(0.9f,0.9f,0.9f));
            y += line_h;
        }

        // --- options with auto wrap (arrow on first line of each option) ---
        y += opt_gap;
        for (int i = 0; i < (int)node->options.size(); ++i) {
            bool sel = (i == selected);
            glm::vec3 color = sel ? glm::vec3(1.0f,0.9f,0.2f) : glm::vec3(0.8f,0.8f,0.8f);

            std::vector<std::string> opt_lines;
            text->wrap_text(node->options[i].label, max_width - 28.0f, opt_lines); // leave room for arrow

            bool first = true;
            for (auto const& ln : opt_lines) {
                if (first) {
                    if (sel) text->draw_text("▶ ", start_x, y, color);
                    else     text->draw_text("  ", start_x, y, color);
                    text->draw_text(ln, start_x + 28.0f, y, color);
                    first = false;
                } else {
                    text->draw_text("  ", start_x, y, color);
                    text->draw_text(ln, start_x + 28.0f, y, color);
                }
                y += line_h;
            }
        }
    } else {
        text->draw_text("Dialogue node not found.", start_x, start_y, glm::vec3(1.0f,0.4f,0.4f));
    }

    text->end();

	// left UI
	draw_sidebar(drawable_size);

	// arena + discs
	draw_arena_and_players(drawable_size);

	// //figure out view transform to center the arena:
	// float aspect = float(drawable_size.x) / float(drawable_size.y);
	// float scale = std::min(
	// 	2.0f * aspect / (Game::ArenaMax.x - Game::ArenaMin.x + 2.0f * Game::PlayerRadius),
	// 	2.0f / (Game::ArenaMax.y - Game::ArenaMin.y + 2.0f * Game::PlayerRadius)
	// );
	// glm::vec2 offset = -0.5f * (Game::ArenaMax + Game::ArenaMin);

	// glm::mat4 world_to_clip = glm::mat4(
	// 	scale / aspect, 0.0f, 0.0f, offset.x,
	// 	0.0f, scale, 0.0f, offset.y,
	// 	0.0f, 0.0f, 1.0f, 0.0f,
	// 	0.0f, 0.0f, 0.0f, 1.0f
	// );

	// {
	// 	DrawLines lines(world_to_clip);

	// 	//helper:
	// 	auto draw_text = [&](glm::vec2 const &at, std::string const &text, float H) {
	// 		lines.draw_text(text,
	// 			glm::vec3(at.x, at.y, 0.0),
	// 			glm::vec3(H, 0.0f, 0.0f), glm::vec3(0.0f, H, 0.0f),
	// 			glm::u8vec4(0x00, 0x00, 0x00, 0x00));
	// 		float ofs = (1.0f / scale) / drawable_size.y;
	// 		lines.draw_text(text,
	// 			glm::vec3(at.x + ofs, at.y + ofs, 0.0),
	// 			glm::vec3(H, 0.0f, 0.0f), glm::vec3(0.0f, H, 0.0f),
	// 			glm::u8vec4(0xff, 0xff, 0xff, 0x00));
	// 	};

	// 	lines.draw(glm::vec3(Game::ArenaMin.x, Game::ArenaMin.y, 0.0f), glm::vec3(Game::ArenaMax.x, Game::ArenaMin.y, 0.0f), glm::u8vec4(0xff, 0x00, 0xff, 0xff));
	// 	lines.draw(glm::vec3(Game::ArenaMin.x, Game::ArenaMax.y, 0.0f), glm::vec3(Game::ArenaMax.x, Game::ArenaMax.y, 0.0f), glm::u8vec4(0xff, 0x00, 0xff, 0xff));
	// 	lines.draw(glm::vec3(Game::ArenaMin.x, Game::ArenaMin.y, 0.0f), glm::vec3(Game::ArenaMin.x, Game::ArenaMax.y, 0.0f), glm::u8vec4(0xff, 0x00, 0xff, 0xff));
	// 	lines.draw(glm::vec3(Game::ArenaMax.x, Game::ArenaMin.y, 0.0f), glm::vec3(Game::ArenaMax.x, Game::ArenaMax.y, 0.0f), glm::u8vec4(0xff, 0x00, 0xff, 0xff));

	// 	for (auto const &player : game.players) {
	// 		glm::u8vec4 col = glm::u8vec4(player.color.x*255, player.color.y*255, player.color.z*255, 0xff);
	// 		if (&player == &game.players.front()) {
	// 			//mark current player (which server sends first):
	// 			lines.draw(
	// 				glm::vec3(player.position + Game::PlayerRadius * glm::vec2(-0.5f,-0.5f), 0.0f),
	// 				glm::vec3(player.position + Game::PlayerRadius * glm::vec2( 0.5f, 0.5f), 0.0f),
	// 				col
	// 			);
	// 			lines.draw(
	// 				glm::vec3(player.position + Game::PlayerRadius * glm::vec2(-0.5f, 0.5f), 0.0f),
	// 				glm::vec3(player.position + Game::PlayerRadius * glm::vec2( 0.5f,-0.5f), 0.0f),
	// 				col
	// 			);
	// 		}
	// 		for (uint32_t a = 0; a < circle.size(); ++a) {
	// 			lines.draw(
	// 				glm::vec3(player.position + Game::PlayerRadius * circle[a], 0.0f),
	// 				glm::vec3(player.position + Game::PlayerRadius * circle[(a+1)%circle.size()], 0.0f),
	// 				col
	// 			);
	// 		}

	// 		draw_text(player.position + glm::vec2(0.0f, -0.1f + Game::PlayerRadius), player.name, 0.09f);
	// 	}
	// }
	GL_ERRORS();
}
