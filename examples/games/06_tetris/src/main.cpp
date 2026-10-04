// ---------------------------------------------------------------------------
// examples/games/06_tetris/src/main.cpp — Tetris, and the honest answer about it.
//
// The sixth judge, and the one included precisely because the framework does
// NOT abstract most of it. A Tetris is a 10x20 array and the rules of Tetris,
// and an rmp::behavior::Tetromino would be the game with a different name on
// it. What the framework contributes is real and it is not the game:
//
//   scenes          play, and a game over pushed on top of it that writes no
//                   policy at all -- the board below freezes because that is
//                   what the defaults do.
//   rmp::ui         the score, and the button that starts again.
//   rmp::input      named actions for move, rotate and soft drop.
//   rmp::random     the 7-bag, and therefore a game that can be replayed from
//                   a seed rather than one that is different every time.
//   rmp::save       the best score, kept between sessions -- three lines to
//                   read it and four to write it, and no `if` on the read:
//                   the first time there is no save, the Value stays empty,
//                   and an empty Value reads as the default. (The write is at
//                   game over, which the CI boot's thirty frames never reach;
//                   rmp::save is tested in tests/save_test.cpp, and on every
//                   platform by the boot itself -- see src/rmp/app.cpp.)
//
// And one thing it does NOT contribute here, said plainly because the
// alternative is a comment that rots: rmp::behavior::GridSnap is not used in
// this file. A Tetris written the way this one is -- integer cells, a piece
// that moves one row at a time -- never has a fractional position to square up.
// GridSnap earns its place where the movement is CONTINUOUS and the result has
// to land on a grid: a Sokoban with a walk animation, a level editor, tower
// placement, a roguelike with smooth steps. It is exercised in
// tests/behavior_test.cpp, including the part that matters -- that it runs
// after the movement and not before it.
//
// The loop below is the rules of Tetris, and that is how it should be.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/input.h>
#include <rmp/random.h>
#include <rmp/save.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

constexpr int WIDE = 10;
constexpr int TALL = 20;
constexpr float CELL = 20;
constexpr Vector2 ORIGIN{ 300, 30 };
constexpr Vector2 PREVIEW{ 72, 132 }; // where the next piece is shown
constexpr int SQUARE = 1; // the O, the one piece that must not turn

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
// screen and stops hearing the keyboard, so this scene writes no policy at all
// -- the pause overlay of examples/scenes/01_stack with another label. The
// focus is what makes ui_accept start again without reaching for the mouse.
template <class Game> class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : _said(said) {}

    void _draw() override {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 190 });
        rmp::ui::begin();
        rmp::ui::text(_said, { .size = rmp::ui::Size::LARGE });
        if (rmp::ui::button("Play again")) rmp::Scene::change<Game>();
        rmp::ui::end();
    }

private:
    const char *_said;
};

class TetrisScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 12, 12, 18, 255 };
        for (auto &row : _board) {
            for (int &cell : row) cell = -1;
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

        _fall += delta * (rmp::input::pressed("move_down") ? 12.0f : 1.0f);
        if (_fall < _step) return;
        _fall = 0;
        if (!try_move(0, 1)) lock_piece();
    }

    void _draw() override {
        // The well. Drawn here rather than as objects: twenty rows of ten
        // cells are two hundred rectangles that never move on their own, and
        // making each one an rmp::Object would be paying for a life cycle none
        // of them has.
        // IN WORLD UNITS, through the camera: _draw() is handed the screen,
        // and drawing the board in screen units would pin it to the top-left
        // corner of a window that is not the design size instead of keeping it
        // where the design put it.
        BeginMode2D(camera.raylib());
        const Rectangle well{ ORIGIN.x, ORIGIN.y, WIDE * CELL, TALL * CELL };
        DrawRectangleRec(well, Color{ 22, 22, 32, 255 });
        DrawRectangleLinesEx(
            Rectangle{ well.x - 4, well.y - 4, well.width + 8, well.height + 8 }, 4,
            rmp::ui::current_theme().border);

        for (int y = 0; y < TALL; y++) {
            for (int x = 0; x < WIDE; x++) {
                if (_board[y][x] < 0) continue;
                draw_cell(x, y, PIECES[_board[y][x]].color);
            }
        }
        for (const auto &cell : _shape) {
            draw_cell(_at[0] + cell[0], _at[1] + cell[1], PIECES[_current].color);
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
            if (y >= 0 && _board[y][x] >= 0) return false;
        }
        return true;
    }

    bool try_move(int dx, int dy) {
        if (!fits(_shape, _at[0] + dx, _at[1] + dy)) return false;
        _at[0] += dx;
        _at[1] += dy;
        return true;
    }

    void try_rotate() {
        // The O is square about a CORNER and not about its middle, so turning
        // its four cells slides the piece one column left. Every Tetris there
        // has ever been leaves it alone, and that is the rule, not a special
        // case: a rotation that moves the piece is not a rotation.
        if (_current == SQUARE) return;
        int turned[4][2];
        for (int i = 0; i < 4; i++) {
            turned[i][0] = -_shape[i][1];
            turned[i][1] = _shape[i][0];
        }
        if (!fits(turned, _at[0], _at[1])) return;
        for (int i = 0; i < 4; i++) {
            _shape[i][0] = turned[i][0];
            _shape[i][1] = turned[i][1];
        }
    }

    void lock_piece() {
        for (const auto &cell : _shape) {
            const int x = _at[0] + cell[0];
            const int y = _at[1] + cell[1];
            if (y < 0) {
                game_over();
                return;
            }
            _board[y][x] = _current;
        }
        clear_lines();
        next_piece();
    }

    void clear_lines() {
        for (int y = TALL - 1; y >= 0; y--) {
            bool full = true;
            for (int x = 0; x < WIDE && full; x++) full = _board[y][x] >= 0;
            if (!full) continue;
            for (int row = y; row > 0; row--) {
                for (int x = 0; x < WIDE; x++) _board[row][x] = _board[row - 1][x];
            }
            for (int x = 0; x < WIDE; x++) _board[0][x] = -1;
            _lines++;
            _step = _step > 0.12f ? _step - 0.01f : _step;
            y++; // the same row again, now that everything fell into it
        }
    }

    // A 7-BAG: every seven pieces ARE the seven pieces, in a shuffled order.
    // Picking one of seven at random gives four S's in a row often enough that
    // every Tetris since 2001 does this instead -- and it comes from
    // rmp::random rather than from raylib's, so the same seed is the same game.
    int from_bag() {
        if (_bag_left == 0) {
            for (int i = 0; i < 7; i++) _bag[i] = i;
            for (int i = 6; i > 0; i--) {
                const int j = rmp::random::index(i + 1);
                const int keep = _bag[i];
                _bag[i] = _bag[j];
                _bag[j] = keep;
            }
            _bag_left = 7;
        }
        return _bag[--_bag_left];
    }

    void next_piece() {
        _current = _next;
        _next = from_bag();
        for (int i = 0; i < 4; i++) {
            _shape[i][0] = PIECES[_current].cells[i][0];
            _shape[i][1] = PIECES[_current].cells[i][1];
        }
        _at[0] = WIDE / 2;
        _at[1] = 0;
        // The other way a Tetris ends: there is no room for what comes next.
        if (!fits(_shape, _at[0], _at[1])) game_over();
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
        rmp::Scene::push<OverScene<TetrisScene>>(record ? "New best!" : "Game over");
    }

    int _board[TALL][WIDE] = {};
    int _shape[4][2] = {};
    int _at[2] = {};
    int _bag[7] = {};
    int _bag_left = 0;
    int _current = 0;
    int _next = 0;
    int _lines = 0;
    int _best = 0;
    float _fall = 0;
    float _step = 0.5f;
};

} // namespace

RMP_GAME(TetrisScene);
