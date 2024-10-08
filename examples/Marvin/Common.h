#pragma once

#include <vector>
#include <string_view>

#include "GameProxy.h"

namespace marvin {

std::string Lowercase(const std::string& str);

std::string GetWorkingDirectory();

bool CheckStatus(GameProxy& game, KeyController& keys, bool use_max);

uint16_t FindOpenFreq(const std::vector<uint16_t>& list, uint16_t start_pos);

std::vector<std::string_view> SplitString(std::string_view string, std::string_view delim);

std::vector<std::string> SplitString(const std::string& string, const std::string& delim);

MapCoord ToNearestTile(Vector2f position);

Vector2f DiscreteToHeading(uint16_t rotation);

float DiscreteToRadians(uint16_t rotation);

std::string GetShip(uint16_t ship);

}  // namespace marvin
