/*
    GSP for the Taurion blockchain game
    Copyright (C) 2019  Autonomous Worlds Ltd

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "pathfinder.hpp"

namespace pxd
{

constexpr PathFinder::DistanceT PathFinder::NO_CONNECTION;

PathFinder::Stepper
PathFinder::StepPath (const HexCoord& source) const
{
  CHECK (distances != nullptr && distances->Get (source) != NO_CONNECTION)
      << "No path from the given source has been computed yet";
  return Stepper (*this, source);
}

PathFinder::DistanceT
PathFinder::Stepper::Next ()
{
  CHECK (HasMore ());

  const auto curDist = finder.distances->Access (position);
  CHECK (curDist != NO_CONNECTION);

  DistanceT bestDist = NO_CONNECTION;
  HexCoord bestNeighbour;
  for (const auto& n : position.Neighbours ())
    {
      if (!finder.distances->IsInRange (n))
        continue;
      const auto dist = finder.distances->Get (n);
      if (dist == NO_CONNECTION)
        continue;
      if (bestDist == NO_CONNECTION || dist < bestDist)
        {
          bestDist = dist;
          bestNeighbour = n;
        }
    }

  CHECK_NE (bestDist, NO_CONNECTION) << "No good neighbour found along path";
  CHECK_LE (bestDist, curDist);

  position = bestNeighbour;
  return curDist - bestDist;
}

} // namespace pxd
