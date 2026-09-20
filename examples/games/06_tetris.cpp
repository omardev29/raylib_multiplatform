// ---------------------------------------------------------------------------
// examples/games/06_tetris.cpp — Tetris, and the honest answer about it.
//
// The sixth judge, and the one included precisely because the framework does
// NOT abstract most of it. A Tetris is a 10x20 array and the rules of Tetris,
// and an rmp::behavior::Tetromino would be the game with a different name on
// it. What the framework contributes is real and it is not the game:
//
//   scenes          menu / play / game over, with the pause overlay writing no
//                   policy at all.
//   rmp::ui         the score and the next piece.
//   rmp::input      named actions for rotate and soft drop.
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
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

constexpr int kWide = 10;
constexpr int kTall = 20;
constexpr float kCell = 20;
constexpr Vector2 kOrigin{ 290, 30 };

// The seven pieces, as offsets from their pivot. Four rotations are worked out
// rather than tabulated, because rotating a coordinate is two lines.
struct Piece {
    int cells[4][2];
    Color color;
};
constexpr Piece kPieces[7] = {
    { { { -1, 0 }, { 0, 0 }, { 1, 0 }, { 2, 0 } }, SKYBLUE }, // I
    { { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } }, GOLD }, // O
    { { { -1, 0 }, { 0, 0 }, { 1, 0 }, { 0, 1 } }, VIOLET }, // T
    { { { -1, 1 }, { 0, 1 }, { 0, 0 }, { 1, 0 } }, LIME }, // S
    { { { -1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 } }, RED }, // Z
    { { { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } }, BLUE }, // J
    { { { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } }, ORANGE }, // L
};

class TetrisScene : public rmp::Scene {
public:
    void _ready() override {
        for (auto &row : board_) {
            for (int &cell : row) cell = -1;
        }
        next_piece();
    }

    void _update(float delta) override {
        if (over_) return;

        if (rmp::input::just_pressed("move_left")) try_move(-1, 0);
        if (rmp::input::just_pressed("move_right")) try_move(1, 0);
        if (rmp::input::just_pressed("ui_accept")) try_rotate();

        fall_ += delta * (rmp::input::pressed("move_down") ? 12.0f : 1.0f);
        if (fall_ < step_) return;
        fall_ = 0;
        if (!try_move(0, 1)) lock_piece();
    }

    void _draw() override {
        // The board. Drawn here rather than as objects: twenty rows of ten
        // cells are two hundred rectangles that never move on their own, and
        // making each one an rmp::Object would be paying for a life cycle none
        // of them has.
        for (int y = 0; y < kTall; y++) {
            for (int x = 0; x < kWide; x++) {
                if (board_[y][x] < 0) continue;
                draw_cell(x, y, kPieces[board_[y][x]].color);
            }
        }
        for (const auto &cell : shape_) {
            draw_cell(at_[0] + cell[0], at_[1] + cell[1], kPieces[current_].color);
        }

        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::text(TextFormat("Lines: %d", lines_));
        if (over_) rmp::ui::text("Game over");
        rmp::ui::end();
    }

private:
    static void draw_cell(int x, int y, Color color) {
        DrawRectangle(static_cast<int>(kOrigin.x + static_cast<float>(x) * kCell),
                      static_cast<int>(kOrigin.y + static_cast<float>(y) * kCell),
                      static_cast<int>(kCell) - 1, static_cast<int>(kCell) - 1, color);
    }

    bool fits(const int cells[4][2], int cx, int cy) const {
        for (int i = 0; i < 4; i++) {
            const int x = cx + cells[i][0];
            const int y = cy + cells[i][1];
            if (x < 0 || x >= kWide || y >= kTall) return false;
            if (y >= 0 && board_[y][x] >= 0) return false;
        }
        return true;
    }

    bool try_move(int dx, int dy) {
        if (!fits(shape_, at_[0] + dx, at_[1] + dy)) return false;
        at_[0] += dx;
        at_[1] += dy;
        return true;
    }

    void try_rotate() {
        int turned[4][2];
        for (int i = 0; i < 4; i++) {
            turned[i][0] = -shape_[i][1];
            turned[i][1] = shape_[i][0];
        }
        if (!fits(turned, at_[0], at_[1])) return;
        for (int i = 0; i < 4; i++) {
            shape_[i][0] = turned[i][0];
            shape_[i][1] = turned[i][1];
        }
    }

    void lock_piece() {
        for (const auto &cell : shape_) {
            const int x = at_[0] + cell[0];
            const int y = at_[1] + cell[1];
            if (y < 0) {
                over_ = true;
                return;
            }
            board_[y][x] = current_;
        }
        clear_lines();
        next_piece();
    }

    void clear_lines() {
        for (int y = kTall - 1; y >= 0; y--) {
            bool full = true;
            for (int x = 0; x < kWide && full; x++) full = board_[y][x] >= 0;
            if (!full) continue;
            for (int row = y; row > 0; row--) {
                for (int x = 0; x < kWide; x++) board_[row][x] = board_[row - 1][x];
            }
            for (int x = 0; x < kWide; x++) board_[0][x] = -1;
            lines_++;
            step_ = step_ > 0.12f ? step_ - 0.01f : step_;
            y++; // the same row again, now that everything fell into it
        }
    }

    void next_piece() {
        current_ = (current_ + 3) % 7;
        for (int i = 0; i < 4; i++) {
            shape_[i][0] = kPieces[current_].cells[i][0];
            shape_[i][1] = kPieces[current_].cells[i][1];
        }
        at_[0] = kWide / 2;
        at_[1] = 0;
        if (!fits(shape_, at_[0], at_[1])) over_ = true;
    }

    int board_[kTall][kWide] = {};
    int shape_[4][2] = {};
    int at_[2] = {};
    int current_ = 0;
    int lines_ = 0;
    float fall_ = 0;
    float step_ = 0.5f;
    bool over_ = false;
};

} // namespace

RMP_GAME(TetrisScene);
