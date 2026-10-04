// ---------------------------------------------------------------------------
// examples/games/06_tetris -- Tetris.
//
// A Tetris is a 10x20 grid and the rules of Tetris, and that is what this file
// is: the framework does not abstract it, because a Tetromino behavior would be
// this game with another name. What the framework does contribute:
//
//   scenes          play, and a game over pushed on top of it; the board below
//                   freezes because that is what the scene stack does.
//   rmp::ui         the score, and the button that starts again.
//   rmp::input      named actions for move, rotate and soft drop.
//   rmp::random     the 7-bag, so the same seed is the same game.
//   rmp::save       the best score, kept between sessions. There is no `if`
//                   around the read: the first time there is no save, and an
//                   empty Value reads as the default.
//
// Run it: `rmp example 06_tetris`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/input.h>
#include <rmp/random.h>
#include <rmp/save.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <utility>

namespace {

constexpr int WIDE = 10;
constexpr int TALL = 20;
constexpr float CELL = 20;
constexpr Vector2 ORIGIN{ 300, 30 };
constexpr Vector2 PREVIEW{ 72, 132 }; // where the next piece is shown
constexpr int SQUARE = 1; // the O, the one piece that must not turn
constexpr int EMPTY = -1; // a cell of the board with nothing in it

// The seven pieces, as offsets from their pivot. Four rotations are worked out
// rather than tabulated, because rotating a coordinate is two lines.
struct Piece {
    int cells[4][2];
    Color color;
};
constexpr Piece PIECES[7] = {
    { { { -1, 0 }, { 0, 0 }, { 1, 0 }, { 2, 0 } }, SKYBLUE }, // I
    { { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } }, GOLD }, // O
    { { { -1, 0 }, { 0, 0 }, { 1, 0 }, { 0, 1 } }, VIOLET }, // T
    { { { -1, 1 }, { 0, 1 }, { 0, 0 }, { 1, 0 } }, LIME }, // S
    { { { -1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 } }, RED }, // Z
    { { { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } }, BLUE }, // J
    { { { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } }, ORANGE }, // L
};

// The end of a game, PUSHED on top of it: the board below freezes, stays on
// screen and stops hearing the keyboard, which is what the scene stack does on
// its own -- so this is only what it says and the way out. Its one button has
// the focus already, so Enter or the gamepad start again too.
class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : _said(said) {}
    void _draw() override; // below TetrisScene, which it starts again

private:
    const char *_said;
};

class TetrisScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 12, 12, 18, 255 };
        for (auto &row : _board) {
            for (int &cell : row) cell = EMPTY;
        }
        _next = from_bag();
        next_piece();

        rmp::Value saved;
        rmp::save::read("tetris", &saved);
        _best = saved["best_lines"].as_int(0);
    }

    void _update(float delta) override {
        if (rmp::input::just_pressed("move_left")) try_move(-1, 0);
        if (rmp::input::just_pressed("move_right")) try_move(1, 0);
        if (rmp::input::just_pressed("ui_accept")) try_rotate();

        // Down a row every _seconds_per_row, twelve times as often while the
        // player holds down.
        _since_last_row += delta * (rmp::input::pressed("move_down") ? 12.0f : 1.0f);
        if (_since_last_row < _seconds_per_row) return;
        _since_last_row = 0;
        if (!try_move(0, 1)) lock_piece();
    }

    void _draw() override {
        // The well, drawn rather than made of objects: two hundred cells that
        // never move on their own do not need an object's life each. Through
        // the camera, in world units, so it stays where the design put it on a
        // window of any size.
        BeginMode2D(camera.raylib());
        const Rectangle well{ ORIGIN.x, ORIGIN.y, WIDE * CELL, TALL * CELL };
        DrawRectangleRec(well, Color{ 22, 22, 32, 255 });
        DrawRectangleLinesEx(
            Rectangle{ well.x - 4, well.y - 4, well.width + 8, well.height + 8 }, 4,
            rmp::ui::current_theme().border);

        for (int y = 0; y < TALL; y++) {
            for (int x = 0; x < WIDE; x++) {
                if (_board[y][x] == EMPTY) continue;
                draw_cell(x, y, PIECES[_board[y][x]].color);
            }
        }
        for (const auto &cell : _shape) {
            draw_cell(_x + cell[0], _y + cell[1], PIECES[_current].color);
        }
        for (const auto &cell : PIECES[_next].cells) {
            draw_block(PREVIEW.x + static_cast<float>(cell[0]) * CELL,
                       PREVIEW.y + static_cast<float>(cell[1]) * CELL,
                       PIECES[_next].color);
        }

        EndMode2D();

        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::text(TextFormat("Lines %d", _lines), { .size = rmp::ui::Size::LARGE });
        rmp::ui::text(TextFormat("Best %d", _best),
                      { .color = rmp::ui::ColorRole::MUTED });
        rmp::ui::text("Next", { .color = rmp::ui::ColorRole::MUTED });
        rmp::ui::end();
    }

private:
    static void draw_block(float x, float y, Color color) {
        DrawRectangle(static_cast<int>(x), static_cast<int>(y),
                      static_cast<int>(CELL) - 1, static_cast<int>(CELL) - 1, color);
    }

    static void draw_cell(int x, int y, Color color) {
        draw_block(ORIGIN.x + static_cast<float>(x) * CELL,
                   ORIGIN.y + static_cast<float>(y) * CELL, color);
    }

    bool fits(const int cells[4][2], int cx, int cy) const {
        for (int i = 0; i < 4; i++) {
            const int x = cx + cells[i][0];
            const int y = cy + cells[i][1];
            if (x < 0 || x >= WIDE || y >= TALL) return false;
            if (y >= 0 && _board[y][x] != EMPTY) return false;
        }
        return true;
    }

    bool try_move(int dx, int dy) {
        if (!fits(_shape, _x + dx, _y + dy)) return false;
        _x += dx;
        _y += dy;
        return true;
    }

    void try_rotate() {
        // The O turns about a corner and not about its middle, so turning it
        // would slide it one column. No Tetris turns it.
        if (_current == SQUARE) return;
        // A quarter turn about the pivot: (x, y) becomes (-y, x).
        int turned[4][2];
        for (int i = 0; i < 4; i++) {
            turned[i][0] = -_shape[i][1];
            turned[i][1] = _shape[i][0];
        }
        if (!fits(turned, _x, _y)) return;
        for (int i = 0; i < 4; i++) {
            _shape[i][0] = turned[i][0];
            _shape[i][1] = turned[i][1];
        }
    }

    void lock_piece() {
        for (const auto &cell : _shape) {
            const int x = _x + cell[0];
            const int y = _y + cell[1];
            if (y < 0) {
                game_over();
                return;
            }
            _board[y][x] = _current;
        }
        clear_lines();
        next_piece();
    }

    // From the bottom up. A full row is removed and everything above falls
    // into it, so the same row is looked at again before moving on.
    void clear_lines() {
        int y = TALL - 1;
        while (y >= 0) {
            if (!row_full(y)) {
                y--;
                continue;
            }
            remove_row(y);
            _lines++;
            if (_seconds_per_row > 0.12f) _seconds_per_row -= 0.01f; // faster, to a limit
        }
    }

    bool row_full(int y) const {
        for (int x = 0; x < WIDE; x++) {
            if (_board[y][x] == EMPTY) return false;
        }
        return true;
    }

    void remove_row(int y) {
        for (int row = y; row > 0; row--) {
            for (int x = 0; x < WIDE; x++) _board[row][x] = _board[row - 1][x];
        }
        for (int x = 0; x < WIDE; x++) _board[0][x] = EMPTY;
    }

    // A 7-bag: every seven pieces are the seven pieces, in a shuffled order.
    // One of seven at random would deal four S's in a row often enough to be
    // unfair. Shuffled with rmp::random, so the same seed is the same game.
    int from_bag() {
        if (_bag_left == 0) {
            for (int i = 0; i < 7; i++) _bag[i] = i;
            for (int i = 6; i > 0; i--)
                std::swap(_bag[i], _bag[rmp::random::index(i + 1)]);
            _bag_left = 7;
        }
        _bag_left--;
        return _bag[_bag_left];
    }

    void next_piece() {
        _current = _next;
        _next = from_bag();
        for (int i = 0; i < 4; i++) {
            _shape[i][0] = PIECES[_current].cells[i][0];
            _shape[i][1] = PIECES[_current].cells[i][1];
        }
        _x = WIDE / 2;
        _y = 0;
        // The other way a Tetris ends: there is no room for what comes next.
        if (!fits(_shape, _x, _y)) game_over();
    }

    // The one number that outlives a game, and the whole of saving it.
    void game_over() {
        const bool record = _lines > _best;
        if (record) {
            _best = _lines;
            rmp::Value v;
            v["best_lines"] = _best;
            rmp::save::write("tetris", v);
        }
        rmp::Scene::push<OverScene>(record ? "New best!" : "Game over");
    }

    int _board[TALL][WIDE] = {}; // EMPTY, or the index of the piece that left it
    int _current = 0; // which of PIECES is falling
    int _shape[4][2] = {}; // its cells, as turned so far
    int _x = 0; // and where its pivot is on the board
    int _y = 0;
    int _next = 0;
    int _bag[7] = {};
    int _bag_left = 0;
    int _lines = 0;
    int _best = 0;
    float _since_last_row = 0;
    float _seconds_per_row = 0.5f;
};

void OverScene::_draw() {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 190 });
    rmp::ui::begin();
    rmp::ui::text(_said, { .size = rmp::ui::Size::LARGE });
    if (rmp::ui::button("Play again")) rmp::Scene::change<TetrisScene>();
    rmp::ui::end();
}

} // namespace

RMP_GAME(TetrisScene);
