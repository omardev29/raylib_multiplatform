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
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

constexpr int kWide = 10;
constexpr int kTall = 20;
constexpr float kCell = 20;
constexpr Vector2 kOrigin{ 300, 30 };
constexpr Vector2 kPreview{ 72, 132 }; // where the next piece is shown
constexpr int kSquare = 1; // the O, the one piece that must not turn

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

// The end of a game, PUSHED on top of it: the board below freezes, stays on
// screen and stops hearing the keyboard, so this scene writes no policy at all
// -- the pause overlay of examples/scenes/01_stack with another label. The
// focus is what makes ui_accept start again without reaching for the mouse.
template <class Game> class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : said_(said) {}

    void _draw() override {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 190 });
        rmp::ui::begin();
        rmp::ui::text(said_, { .size = rmp::ui::Size::LARGE });
        if (rmp::ui::button("Play again")) rmp::Scene::change<Game>();
        rmp::ui::end();
    }

private:
    const char *said_;
};

class TetrisScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 12, 12, 18, 255 };
        for (auto &row : board_) {
            for (int &cell : row) cell = -1;
        }
        next_ = from_bag();
        next_piece();
    }

    void _update(float delta) override {
        if (rmp::input::just_pressed("move_left")) try_move(-1, 0);
        if (rmp::input::just_pressed("move_right")) try_move(1, 0);
        if (rmp::input::just_pressed("ui_accept")) try_rotate();

        fall_ += delta * (rmp::input::pressed("move_down") ? 12.0f : 1.0f);
        if (fall_ < step_) return;
        fall_ = 0;
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
        const Rectangle well{ kOrigin.x, kOrigin.y, kWide * kCell, kTall * kCell };
        DrawRectangleRec(well, Color{ 22, 22, 32, 255 });
        DrawRectangleLinesEx(
            Rectangle{ well.x - 4, well.y - 4, well.width + 8, well.height + 8 }, 4,
            rmp::ui::current_theme().border);

        for (int y = 0; y < kTall; y++) {
            for (int x = 0; x < kWide; x++) {
                if (board_[y][x] < 0) continue;
                draw_cell(x, y, kPieces[board_[y][x]].color);
            }
        }
        for (const auto &cell : shape_) {
            draw_cell(at_[0] + cell[0], at_[1] + cell[1], kPieces[current_].color);
        }
        for (const auto &cell : kPieces[next_].cells) {
            draw_block(kPreview.x + static_cast<float>(cell[0]) * kCell,
                       kPreview.y + static_cast<float>(cell[1]) * kCell,
                       kPieces[next_].color);
        }

        EndMode2D();

        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::text(TextFormat("Lines %d", lines_), { .size = rmp::ui::Size::LARGE });
        rmp::ui::text("Next", { .color = rmp::ui::ColorRole::MUTED });
        rmp::ui::end();
    }

private:
    static void draw_block(float x, float y, Color color) {
        DrawRectangle(static_cast<int>(x), static_cast<int>(y),
                      static_cast<int>(kCell) - 1, static_cast<int>(kCell) - 1, color);
    }

    static void draw_cell(int x, int y, Color color) {
        draw_block(kOrigin.x + static_cast<float>(x) * kCell,
                   kOrigin.y + static_cast<float>(y) * kCell, color);
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
        // The O is square about a CORNER and not about its middle, so turning
        // its four cells slides the piece one column left. Every Tetris there
        // has ever been leaves it alone, and that is the rule, not a special
        // case: a rotation that moves the piece is not a rotation.
        if (current_ == kSquare) return;
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
                rmp::Scene::push<OverScene<TetrisScene>>("Game over");
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

    // A 7-BAG: every seven pieces ARE the seven pieces, in a shuffled order.
    // Picking one of seven at random gives four S's in a row often enough that
    // every Tetris since 2001 does this instead -- and it comes from
    // rmp::random rather than from raylib's, so the same seed is the same game.
    int from_bag() {
        if (bag_left_ == 0) {
            for (int i = 0; i < 7; i++) bag_[i] = i;
            for (int i = 6; i > 0; i--) {
                const int j = rmp::random::index(i + 1);
                const int keep = bag_[i];
                bag_[i] = bag_[j];
                bag_[j] = keep;
            }
            bag_left_ = 7;
        }
        return bag_[--bag_left_];
    }

    void next_piece() {
        current_ = next_;
        next_ = from_bag();
        for (int i = 0; i < 4; i++) {
            shape_[i][0] = kPieces[current_].cells[i][0];
            shape_[i][1] = kPieces[current_].cells[i][1];
        }
        at_[0] = kWide / 2;
        at_[1] = 0;
        // The other way a Tetris ends: there is no room for what comes next.
        if (!fits(shape_, at_[0], at_[1])) {
            rmp::Scene::push<OverScene<TetrisScene>>("Game over");
        }
    }

    int board_[kTall][kWide] = {};
    int shape_[4][2] = {};
    int at_[2] = {};
    int bag_[7] = {};
    int bag_left_ = 0;
    int current_ = 0;
    int next_ = 0;
    int lines_ = 0;
    float fall_ = 0;
    float step_ = 0.5f;
};

} // namespace

RMP_GAME(TetrisScene);
