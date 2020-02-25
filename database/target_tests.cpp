/*
    GSP for the Taurion blockchain game
    Copyright (C) 2019-2020  Autonomous Worlds Ltd

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

#include "target.hpp"

#include "building.hpp"
#include "character.hpp"
#include "dbtest.hpp"

#include "hexagonal/coord.hpp"
#include "proto/combat.pb.h"

#include <gtest/gtest.h>

#include <vector>

namespace pxd
{
namespace
{

class TargetFinderTests : public DBTestWithSchema
{

protected:

  BuildingsTable buildings;
  CharacterTable characters;

  TargetFinder finder;

  /**
   * Vector into which found targets will be inserted by the constructed
   * callback function cb.
   */
  std::vector<std::pair<HexCoord, proto::TargetId>> found;

  /** Callback that inserts targets into found.  */
  TargetFinder::ProcessingFcn cb;

  TargetFinderTests ()
    : buildings(db), characters(db), finder(db)
  {
    cb = [this] (const HexCoord& c, const proto::TargetId& t)
      {
        found.emplace_back (c, t);
      };
  }

  /**
   * Inserts a test building at the given centre and with the given faction.
   * Returns the ID.
   */
  Database::IdT
  InsertBuilding (const HexCoord& pos, const Faction faction)
  {
    auto h = buildings.CreateNew ("checkmark", "", faction);
    h->SetCentre (pos);

    return h->GetId ();
  }

  /**
   * Inserts a test character at the given position and with the given faction.
   * Returns the ID.
   */
  Database::IdT
  InsertCharacter (const HexCoord& pos, const Faction faction)
  {
    auto h = characters.CreateNew ("domob", faction);
    h->SetPosition (pos);

    return h->GetId ();
  }

};

TEST_F (TargetFinderTests, CharacterFactions)
{
  const auto idEnemy1 = InsertCharacter (HexCoord (1, 1), Faction::GREEN);
  InsertCharacter (HexCoord (-1, 1), Faction::RED);
  const auto idEnemy2 = InsertCharacter (HexCoord (0, 0), Faction::BLUE);

  finder.ProcessL1Targets (HexCoord (0, 0), 2, Faction::RED, cb);

  ASSERT_EQ (found.size (), 2);

  EXPECT_EQ (found[0].first, HexCoord (1, 1));
  EXPECT_EQ (found[0].second.type (), proto::TargetId::TYPE_CHARACTER);
  EXPECT_EQ (found[0].second.id (), idEnemy1);

  EXPECT_EQ (found[1].first, HexCoord (0, 0));
  EXPECT_EQ (found[1].second.type (), proto::TargetId::TYPE_CHARACTER);
  EXPECT_EQ (found[1].second.id (), idEnemy2);
}

TEST_F (TargetFinderTests, InBuilding)
{
  characters.CreateNew ("domob", Faction::GREEN)->SetBuildingId (100);
  finder.ProcessL1Targets (HexCoord (0, 0), 1, Faction::RED, cb);
  EXPECT_TRUE (found.empty ());
}

TEST_F (TargetFinderTests, CharacterRange)
{
  const HexCoord centre(10, -15);
  const HexCoord::IntT range = 5;

  std::vector<std::pair<Database::IdT, HexCoord>> expected;
  for (HexCoord::IntT x = centre.GetX () - 2 * range;
       x <= centre.GetX () + 2 * range; ++x)
    for (HexCoord::IntT y = centre.GetY () - 2 * range;
         y <= centre.GetY () + 2 * range; ++y)
      {
        const HexCoord pos(x, y);
        const auto id = InsertCharacter (pos, Faction::GREEN);

        if (HexCoord::DistanceL1 (pos, centre) <= range)
          expected.emplace_back (id, pos);
      }

  finder.ProcessL1Targets (centre, range, Faction::RED, cb);

  ASSERT_EQ (found.size (), expected.size ());
  for (unsigned i = 0; i < expected.size (); ++i)
    {
      EXPECT_EQ (found[i].first, expected[i].second);
      EXPECT_EQ (found[i].second.type (), proto::TargetId::TYPE_CHARACTER);
      EXPECT_EQ (found[i].second.id (), expected[i].first);
    }
}

TEST_F (TargetFinderTests, BuildingFactions)
{
  const HexCoord pos(10, -15);

  InsertBuilding (pos, Faction::ANCIENT);
  InsertBuilding (pos, Faction::RED);
  const auto idEnemy1 = InsertBuilding (pos, Faction::GREEN);
  const auto idEnemy2 = InsertBuilding (pos, Faction::BLUE);

  finder.ProcessL1Targets (pos, 1, Faction::RED, cb);

  ASSERT_EQ (found.size (), 2);

  EXPECT_EQ (found[0].first, pos);
  EXPECT_EQ (found[0].second.type (), proto::TargetId::TYPE_BUILDING);
  EXPECT_EQ (found[0].second.id (), idEnemy1);

  EXPECT_EQ (found[1].first, pos);
  EXPECT_EQ (found[1].second.type (), proto::TargetId::TYPE_BUILDING);
  EXPECT_EQ (found[1].second.id (), idEnemy2);
}

TEST_F (TargetFinderTests, BuildingsAndCharacters)
{
  const HexCoord pos(10, -15);

  const auto building1 = InsertBuilding (pos, Faction::GREEN);
  const auto char1 = InsertCharacter (pos, Faction::BLUE);
  const auto building2 = InsertBuilding (pos, Faction::GREEN);
  const auto char2 = InsertCharacter (pos, Faction::BLUE);

  finder.ProcessL1Targets (pos, 1, Faction::RED, cb);

  ASSERT_EQ (found.size (), 4);

  EXPECT_EQ (found[0].second.type (), proto::TargetId::TYPE_BUILDING);
  EXPECT_EQ (found[0].second.id (), building1);
  EXPECT_EQ (found[1].second.type (), proto::TargetId::TYPE_BUILDING);
  EXPECT_EQ (found[1].second.id (), building2);

  EXPECT_EQ (found[2].second.type (), proto::TargetId::TYPE_CHARACTER);
  EXPECT_EQ (found[2].second.id (), char1);
  EXPECT_EQ (found[3].second.type (), proto::TargetId::TYPE_CHARACTER);
  EXPECT_EQ (found[3].second.id (), char2);
}

} // anonymous namespace
} // namespace pxd
