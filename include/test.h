#pragma once
#include <raylib.h>

#include "assets.h"

class GameAssets {
public:
  Texture2D rabbit;
  Image img;
};

inline GameAssets LoadGameAssets() {
  // Loads from resources.rres when a pack is present, otherwise from the loose
  // file. See assets.h / Assets::Init().
  Image img = Assets::LoadImage("rabbit.png");
  Texture2D tex = LoadTextureFromImage(img);
  return {tex, img};
}
