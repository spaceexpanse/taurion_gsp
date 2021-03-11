/*
    GSP for the Taurion blockchain game
    Copyright (C) 2019-2021  Autonomous Worlds Ltd

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

#include "logic.hpp"

#include "fame_tests.hpp"
#include "jsonutils.hpp"
#include "params.hpp"
#include "protoutils.hpp"
#include "testutils.hpp"

#include "database/account.hpp"
#include "database/building.hpp"
#include "database/character.hpp"
#include "database/damagelists.hpp"
#include "database/dbtest.hpp"
#include "database/dex.hpp"
#include "database/faction.hpp"
#include "database/inventory.hpp"
#include "database/ongoing.hpp"
#include "database/region.hpp"
#include "hexagonal/coord.hpp"
#include "mapdata/basemap.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <json/json.h>

#include <string>
#include <vector>

namespace pxd
{

/* ************************************************************************** */

/**
 * Test fixture for testing PXLogic::UpdateState.  It sets up a test database
 * independent from SQLiteGame, so that we can more easily test custom
 * situations as needed.
 */
class PXLogicTests : public DBTestWithSchema
{

private:

  TestRandom rnd;

protected:

  ContextForTesting ctx;

  AccountsTable accounts;
  BuildingsTable buildings;
  CharacterTable characters;
  DexOrderTable orders;
  BuildingInventoriesTable inv;
  GroundLootTable groundLoot;
  OngoingsTable ongoings;
  RegionsTable regions;

  PXLogicTests ()
    : accounts(db), buildings(db), characters(db), orders(db),
      inv(db), groundLoot(db), ongoings(db), regions(db, 0)
  {
    SetHeight (42);
  }

  /**
   * Builds a blockData JSON value from the given moves.
   */
  Json::Value
  BuildBlockData (const Json::Value& moves)
  {
    Json::Value blockData(Json::objectValue);
    blockData["admin"] = Json::Value (Json::arrayValue);
    blockData["moves"] = moves;

    Json::Value meta(Json::objectValue);
    meta["height"] = ctx.Height ();
    meta["timestamp"] = 1500000000;
    blockData["block"] = meta;

    return blockData;
  }

  /**
   * Creates a new character for the given name and faction.  Initialises
   * the account in the database first as needed.
   */
  CharacterTable::Handle
  CreateCharacter (const std::string& name, const Faction f)
  {
    auto a = accounts.GetByName (name);
    if (a == nullptr)
      {
        a = accounts.CreateNew (name);
        a->SetFaction (f);
      }

    CHECK (a->GetFaction () == f);
    return characters.CreateNew (name, f);
  }

  /**
   * Creates a new building and sets up also the age data and other things
   * so it is considered valid.
   */
  BuildingsTable::Handle
  CreateBuilding (const std::string& type, const std::string& owner,
                  const Faction f)
  {
    auto b = buildings.CreateNew (type, owner, f);
    auto* age = b->MutableProto ().mutable_age_data ();
    age->set_founded_height (0);
    age->set_finished_height (0);
    return b;
  }

  /**
   * Sets the block height for processing the next block.
   */
  void
  SetHeight (const unsigned h)
  {
    ctx.SetHeight (h);
    regions.SetHeightForTesting (h);
  }

  /**
   * Calls PXLogic::UpdateState with our test instances of the database,
   * params and RNG.  The given string is parsed as JSON array and used
   * as moves in the block data.
   */
  void
  UpdateState (const std::string& movesStr)
  {
    UpdateStateJson (ParseJson (movesStr));
  }

  /**
   * Updates the state as with UpdateState, but with moves given
   * already as JSON value.
   */
  void
  UpdateStateJson (const Json::Value& moves)
  {
    UpdateStateWithData (BuildBlockData (moves));
  }

  /**
   * Calls PXLogic::UpdateState with the given block data and our params, RNG
   * and stuff.  This is a more general variant of UpdateState(std::string),
   * where the block data can be modified to include extra stuff (e.g. a block
   * height of our choosing).
   */
  void
  UpdateStateWithData (const Json::Value& blockData)
  {
    PXLogic::UpdateState (db, rnd, ctx.Chain (), ctx.Map (), blockData);
  }

  /**
   * Calls PXLogic::UpdateState with the given moves and a provided (mocked)
   * FameUpdater instance.
   */
  void
  UpdateStateWithFame (FameUpdater& fame, const std::string& moveStr)
  {
    const auto blockData = BuildBlockData (ParseJson (moveStr));
    PXLogic::UpdateState (db, fame, rnd, ctx, blockData);
  }

  /**
   * Calls game-state validation.
   */
  void
  ValidateState ()
  {
    PXLogic::ValidateStateSlow (db, ctx);
  }

};

namespace
{

/**
 * Adds an attack to the character that does always exactly one damage and
 * has the given range.
 */
void
AddUnityAttack (Character& c, const HexCoord::IntT range)
{
  auto* attack = c.MutableProto ().mutable_combat_data ()->add_attacks ();
  attack->set_range (range);
  attack->mutable_damage ()->set_min (1);
  attack->mutable_damage ()->set_max (1);
}

/* ************************************************************************** */

TEST_F (PXLogicTests, WaypointsBeforeMovement)
{
  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 1);
  c->MutableVolatileMv ().set_partial_step (1000);
  auto& pb = c->MutableProto ();
  pb.set_speed (750);
  pb.mutable_combat_data ();
  pb.mutable_movement ()->add_waypoints ()->set_x (5);
  c.reset ();

  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 1, "wp": )" + WpStr ({HexCoord (-1, 0)}) + R"(}}
    }
  ])");

  EXPECT_EQ (characters.GetById (1)->GetPosition (), HexCoord (0, 0));
  UpdateState ("[]");
  EXPECT_EQ (characters.GetById (1)->GetPosition (), HexCoord (-1, 0));
}

TEST_F (PXLogicTests, MovementBeforeTargeting)
{
  auto c = CreateCharacter ("domob", Faction::RED);
  const auto id1 = c->GetId ();
  AddUnityAttack (*c, 10);
  c.reset ();

  c = CreateCharacter ("andy", Faction::GREEN);
  const auto id2 = c->GetId ();
  c->SetPosition (HexCoord (11, 0));
  auto& pb = c->MutableProto ();
  pb.set_speed (750);
  pb.mutable_combat_data ();
  c.reset ();

  UpdateState ("[]");

  ASSERT_EQ (characters.GetById (id2)->GetPosition (), HexCoord (11, 0));
  ASSERT_FALSE (characters.GetById (id1)->HasTarget ());

  c = characters.GetById (id2);
  auto* wp = c->MutableProto ().mutable_movement ()->mutable_waypoints ();
  c->MutableVolatileMv ().set_partial_step (500);
  *wp->Add () = CoordToProto (HexCoord (0, 0));
  c.reset ();

  UpdateState ("[]");

  ASSERT_EQ (characters.GetById (id2)->GetPosition (), HexCoord (10, 0));
  c = characters.GetById (id1);
  const auto& t = c->GetTarget ();
  EXPECT_EQ (t.type (), proto::TargetId::TYPE_CHARACTER);
  EXPECT_EQ (t.id (), id2);
}

TEST_F (PXLogicTests, KilledVehicleNoLongerBlocks)
{
  auto c = CreateCharacter ("attacker", Faction::GREEN);
  const auto idAttacker = c->GetId ();
  c->SetPosition (HexCoord (11, 0));
  AddUnityAttack (*c, 1);
  c.reset ();

  c = CreateCharacter ("obstacle", Faction::RED);
  const auto idObstacle = c->GetId ();
  c->SetPosition (HexCoord (10, 0));
  c->MutableHP ().set_armour (1);
  c->MutableProto ().mutable_combat_data ();
  c.reset ();

  c = CreateCharacter ("moving", Faction::RED);
  const auto idMoving = c->GetId ();
  c->SetPosition (HexCoord (9, 0));
  auto& pb = c->MutableProto ();
  pb.set_speed (1000);
  pb.mutable_combat_data ();
  c.reset ();

  /* Process one block to allow targeting.  */
  UpdateState ("[]");
  ASSERT_NE (characters.GetById (idObstacle), nullptr);
  ASSERT_EQ (characters.GetById (idAttacker)->GetTarget ().id (), idObstacle);

  /* Next block, the obstacle should be killed and the moving vehicle
     can be moved into its spot.  */
  ASSERT_EQ (idMoving, 3);
  UpdateState (R"([
    {
      "name": "moving",
      "move": {"c": {"id": 3, "wp": )" + WpStr ({HexCoord (10, 0)}) + R"(}}
    }
  ])");

  ASSERT_EQ (characters.GetById (idObstacle), nullptr);
  EXPECT_EQ (characters.GetById (idMoving)->GetPosition (), HexCoord (10, 0));
}

TEST_F (PXLogicTests, NewBuildingBlocksMovement)
{
  auto c = CreateCharacter ("builder", Faction::GREEN);
  ASSERT_EQ (c->GetId (), 1);
  c->SetPosition (HexCoord (0, 0));
  c->MutableProto ().set_cargo_space (1'000);
  c->GetInventory ().AddFungibleCount ("foo", 10);
  c.reset ();

  c = CreateCharacter ("moving", Faction::RED);
  ASSERT_EQ (c->GetId (), 2);
  c->SetPosition (HexCoord (1, 0));
  c->MutableProto ().set_speed (1'000);
  c.reset ();

  /* Building a foundation should immediately block future movement in the
     same round already.  */
  db.SetNextId (101);
  UpdateState (R"([
    {
      "name": "moving",
      "move": {"c": {"id": 2, "wp": )" + WpStr ({HexCoord (0, 0)}) + R"(}}
    },
    {
      "name": "builder",
      "move": {"c": {"id": 1, "fb": {"t": "huesli", "rot": 0}}}
    }
  ])");

  EXPECT_NE (buildings.GetById (101), nullptr);
  EXPECT_EQ (characters.GetById (2)->GetPosition (), HexCoord (1, 0));
}

TEST_F (PXLogicTests, DamageInNextRound)
{
  auto c = CreateCharacter ("domob", Faction::RED);
  AddUnityAttack (*c, 1);
  c.reset ();

  c = CreateCharacter ("andy", Faction::GREEN);
  const auto idTarget = c->GetId ();
  c->MutableHP ().set_armour (100);
  c->MutableProto ().mutable_combat_data ();
  c.reset ();

  UpdateState ("[]");
  EXPECT_EQ (characters.GetById (idTarget)->GetHP ().armour (), 100);
  UpdateState ("[]");
  EXPECT_EQ (characters.GetById (idTarget)->GetHP ().armour (), 99);
}

TEST_F (PXLogicTests, DamageKillsRegeneration)
{
  auto c = CreateCharacter ("domob", Faction::RED);
  AddUnityAttack (*c, 1);
  c.reset ();

  c = CreateCharacter ("andy", Faction::GREEN);
  const auto idTarget = c->GetId ();
  c->MutableProto ().mutable_combat_data ();
  c.reset ();

  /* Progress one round forward to target.  */
  UpdateState ("[]");

  /* Update the target character so that it will be killed with the attack,
     but would regenerate HP if that were done before applying damage.  */
  c = characters.GetById (idTarget);
  ASSERT_TRUE (c != nullptr);
  auto& regen = c->MutableRegenData ();
  regen.mutable_regeneration_mhp ()->set_shield (2'000);
  regen.mutable_max_hp ()->set_shield (100);
  c->MutableHP ().set_shield (1);
  c->MutableHP ().set_armour (0);
  c.reset ();

  /* Now the attack should kill the target before it can regenerate.  */
  UpdateState ("[]");
  EXPECT_TRUE (characters.GetById (idTarget) == nullptr);
}

TEST_F (PXLogicTests, RangeReduction)
{
  /* Place two range reducers just in range of each other.  Due to the delay
     with which the effect is done (targeting first, effect next), this will
     lead to alternation of them hitting each other and the effect being on
     and they being out of range.  */

  auto c = CreateCharacter ("domob", Faction::RED);
  const auto id1 = c->GetId ();
  c->SetPosition (HexCoord (0, 0));
  auto* attack = c->MutableProto ().mutable_combat_data ()->add_attacks ();
  attack->set_area (10);
  attack->mutable_effects ()->mutable_range ()->set_percent (-10);
  c.reset ();

  c = CreateCharacter ("andy", Faction::GREEN);
  const auto id2 = c->GetId ();
  c->SetPosition (HexCoord (10, 0));
  attack = c->MutableProto ().mutable_combat_data ()->add_attacks ();
  attack->set_area (10);
  attack->mutable_effects ()->mutable_range ()->set_percent (-10);
  c.reset ();

  /* Process one initial round for targeting (the effect is off), which
     brings us into the cycle that will then be repeated.  */
  UpdateState ("[]");

  for (unsigned i = 0; i < 10; ++i)
    {
      UpdateState ("[]");
      for (const auto id : {id1, id2})
        {
          c = characters.GetById (id);
          EXPECT_TRUE (c->GetEffects ().has_range ());
          EXPECT_FALSE (c->HasTarget ());
          c.reset ();
        }

      UpdateState ("[]");
      for (const auto id : {id1, id2})
        {
          c = characters.GetById (id);
          EXPECT_FALSE (c->GetEffects ().has_range ());
          EXPECT_TRUE (c->HasTarget ());
          c.reset ();
        }
    }
}

TEST_F (PXLogicTests, MenteconAlteration)
{
  /* If two friendlies with a mentecon each are placed in each other's range
     and one of the mentecons is activated, they will alternatively hit each
     other and have their mentecon effects active / inactive.

     If both are activated at the same time, then they will just stay active
     all the time.  */

  auto c = CreateCharacter ("trigger", Faction::GREEN);
  ASSERT_EQ (c->GetId (), 1);
  c->SetPosition (HexCoord (-10, -1));
  c->MutableProto ().set_speed (1'000);
  auto* attack = c->MutableProto ().mutable_combat_data ()->add_attacks ();
  attack->set_range (10);
  attack->mutable_effects ()->set_mentecon (true);
  c.reset ();

  std::vector<Database::IdT> ids;
  for (unsigned i = 0; i < 2; ++i)
    {
      c = CreateCharacter ("domob", Faction::RED);
      ids.push_back (c->GetId ());
      c->SetPosition (HexCoord (i, 0));
      AddUnityAttack (*c, 5);
      auto* attack = c->MutableProto ().mutable_combat_data ()->add_attacks ();
      attack->set_range (10);
      attack->mutable_effects ()->set_mentecon (true);
      c->MutableHP ().set_shield (0);
      c->MutableHP ().set_armour (100);
      c.reset ();
    }

  /* The effect is off initially and the trigger out of range, which means they
     should not start affecting each other.  */
  for (unsigned i = 0; i < 10; ++i)
    UpdateState ("[]");
  for (const auto id : ids)
    {
      c = characters.GetById (id);
      EXPECT_EQ (c->GetHP ().armour (), 100);
      EXPECT_FALSE (c->HasTarget ());
      EXPECT_FALSE (c->GetEffects ().mentecon ());
      c.reset ();
    }

  /* Move the trigger unit so that it will be in range of id0 for one block
     (which is enough to trigger that unit's mentecon effect and starting
     the alteration).  */
  UpdateState (R"([
    {
      "name": "trigger",
      "move": {"c": {"id": 1, "wp": )"
        + WpStr ({HexCoord (-10, 0), HexCoord (-11, 1)})
        + R"(}}
    }
  ])");
  UpdateState ("[]");
  for (unsigned i = 0; i < 10; ++i)
    for (unsigned j = 0; j < 2; ++j)
      {
        const auto thisId = ids[j];
        const auto otherId = ids[1 - j];

        c = characters.GetById (thisId);
        EXPECT_TRUE (c->GetEffects ().mentecon ());
        EXPECT_TRUE (c->HasTarget ());
        c.reset ();

        c = characters.GetById (otherId);
        EXPECT_FALSE (c->GetEffects ().mentecon ());
        EXPECT_FALSE (c->HasTarget ());
        c.reset ();

        UpdateState ("[]");
      }
  for (const auto id : ids)
    EXPECT_EQ (characters.GetById (id)->GetHP ().armour (), 90);

  /* Do the same trigger movement again (except in reverse for simplicity).
     After the empty update, id1 will be targeting id0.  So after the movement
     block, id0 will be targeting id1, and the trigger will be targeting id0.
     After another block, both will continuously have the mentecon active
     and just keep hitting each other all the time.  */
  UpdateState ("[]");
  UpdateState (R"([
    {
      "name": "trigger",
      "move": {"c": {"id": 1, "wp": )"
        + WpStr ({HexCoord (-10, 0), HexCoord (-10, -1)})
        + R"(}}
    }
  ])");
  for (unsigned i = 0; i < 10; ++i)
    {
      UpdateState ("[]");
      for (const auto id : ids)
        {
          c = characters.GetById (id);
          EXPECT_TRUE (c->HasTarget ());
          EXPECT_TRUE (c->GetEffects ().mentecon ());
          c.reset ();
        }
    }
  /* During the first empty block, id0 hits id1.  During the movement block,
     id1 hits id0.  During the first empty update the loop, id0 hits id1.
     For all nine remaining updates, both hit each other.  */
  EXPECT_EQ (characters.GetById (ids[0])->GetHP ().armour (), 80);
  EXPECT_EQ (characters.GetById (ids[1])->GetHP ().armour (), 79);
}

TEST_F (PXLogicTests, PickUpDeadDrop)
{
  const HexCoord pos(10, 20);

  auto c = CreateCharacter ("attacker", Faction::RED);
  const auto idAttacker = c->GetId ();
  ASSERT_EQ (idAttacker, 1);
  c->SetPosition (pos);
  c->MutableProto ().set_cargo_space (1000);
  AddUnityAttack (*c, 1);
  c.reset ();

  c = CreateCharacter ("target", Faction::GREEN);
  const auto idTarget = c->GetId ();
  c->SetPosition (pos);
  c->MutableProto ().set_cargo_space (1000);
  c->MutableProto ().mutable_combat_data ();
  c->GetInventory ().SetFungibleCount ("foo", 10);
  c.reset ();

  /* Progress one round forward to target.  */
  UpdateState ("[]");

  /* Update the target character so that it will be killed with the attack.  */
  c = characters.GetById (idTarget);
  ASSERT_TRUE (c != nullptr);
  c->MutableHP ().set_shield (1);
  c->MutableHP ().set_armour (0);
  c.reset ();

  /* Now the attack should kill the target.  The attacker should be able to
     pick up the dropped loot right at the same time, because kills are
     processed at the beginning of a block, before handling moves.  */
  UpdateState (R"([
    {
      "name": "attacker",
      "move": {"c": {"id": 1, "pu": {"f": {"foo": 3}}}}
    }
  ])");

  EXPECT_TRUE (characters.GetById (idTarget) == nullptr);
  c = characters.GetById (idAttacker);
  ASSERT_TRUE (c != nullptr);
  EXPECT_EQ (c->GetInventory ().GetFungibleCount ("foo"), 3);
}

TEST_F (PXLogicTests, DamageLists)
{
  DamageLists dl(db, 0);

  auto c = CreateCharacter ("domob", Faction::RED);
  const auto idAttacker = c->GetId ();
  AddUnityAttack (*c, 1);
  c.reset ();

  c = CreateCharacter ("andy", Faction::GREEN);
  const auto idTarget = c->GetId ();
  c->MutableProto ().mutable_combat_data ();
  auto& regen = c->MutableRegenData ();
  regen.mutable_max_hp ()->set_shield (100);
  c->MutableHP ().set_shield (100);
  c.reset ();

  /* Progress one round forward to target.  */
  UpdateState ("[]");

  /* Deal damage, which should be recorded in the damage list.  */
  Json::Value blockData = BuildBlockData (ParseJson ("[]"));
  blockData["block"]["height"] = 100;
  UpdateStateWithData (blockData);
  EXPECT_EQ (dl.GetAttackers (idTarget),
             DamageLists::Attackers ({idAttacker}));

  /* Remove the attacks, so the damage list entry is not refreshed.  */
  c = characters.GetById (idAttacker);
  c->MutableProto ().mutable_combat_data ()->clear_attacks ();
  c.reset ();

  /* The damage list entry should still be present 99 blocks after.  */
  blockData = BuildBlockData (ParseJson ("[]"));
  blockData["block"]["height"] = 199;
  UpdateStateWithData (blockData);
  EXPECT_EQ (dl.GetAttackers (idTarget),
             DamageLists::Attackers ({idAttacker}));

  /* The entry should be removed at block 200.  */
  blockData = BuildBlockData (ParseJson ("[]"));
  blockData["block"]["height"] = 200;
  UpdateStateWithData (blockData);
  EXPECT_EQ (dl.GetAttackers (idTarget), DamageLists::Attackers ({}));
}

TEST_F (PXLogicTests, FameUpdate)
{
  /* Set up two characters that will kill each other in the same turn.  */
  std::vector<Database::IdT> ids;
  for (const auto f : {Faction::RED, Faction::GREEN})
    {
      auto c = CreateCharacter (FactionToString (f), f);
      ids.push_back (c->GetId ());
      AddUnityAttack (*c, 1);
      auto& regen = c->MutableRegenData ();
      regen.mutable_max_hp ()->set_shield (1);
      c->MutableHP ().set_shield (1);
    }

  MockFameUpdater fame(db, ctx);

  EXPECT_CALL (fame, UpdateForKill (ids[0], DamageLists::Attackers ({ids[1]})));
  EXPECT_CALL (fame, UpdateForKill (ids[1], DamageLists::Attackers ({ids[0]})));

  /* Do two updates, first for targeting and the second for the actual kill.  */
  UpdateStateWithFame (fame, "[]");
  ASSERT_NE (characters.GetById (ids[0]), nullptr);
  ASSERT_NE (characters.GetById (ids[1]), nullptr);
  UpdateStateWithFame (fame, "[]");
  ASSERT_EQ (characters.GetById (ids[0]), nullptr);
  ASSERT_EQ (characters.GetById (ids[1]), nullptr);
}

TEST_F (PXLogicTests, CombatEffectRetarder)
{
  /* In this test, we set up a red character with retarding attack, and
     move both a green enemy and a red friendly through the AoE of that
     attack.  The retarding area is from 0 to 10 at y=-10 (for the green)
     and from -10 to 0 at y=10 (for the red).  */

  auto c = CreateCharacter ("domob", Faction::RED);
  c->SetPosition (HexCoord (0, 0));
  auto& attack = *c->MutableProto ().mutable_combat_data ()->add_attacks ();
  attack.set_range (10);
  attack.mutable_effects ()->mutable_speed ()->set_percent (-50);
  c.reset ();

  c = CreateCharacter ("andy", Faction::GREEN);
  const auto idTarget = c->GetId ();
  c->MutableProto ().mutable_combat_data ();
  c->MutableProto ().set_speed (2'000);
  *c->MutableProto ().mutable_movement ()->add_waypoints ()
      = CoordToProto (HexCoord (20, -10));
  c->SetPosition (HexCoord (-10, -10));
  c.reset ();

  c = CreateCharacter ("other", Faction::RED);
  const auto idFriendly = c->GetId ();
  c->MutableProto ().mutable_combat_data ();
  c->MutableProto ().set_speed (2'000);
  *c->MutableProto ().mutable_movement ()->add_waypoints ()
      = CoordToProto (HexCoord (10, 10));
  c->SetPosition (HexCoord (-20, 10));
  c.reset ();

  for (unsigned i = 0; i < 5; ++i)
    UpdateState ("[]");
  EXPECT_EQ (characters.GetById (idTarget)->GetPosition (), HexCoord (0, -10));
  UpdateState ("[]");
  EXPECT_EQ (characters.GetById (idTarget)->GetPosition (), HexCoord (1, -10));
  EXPECT_EQ (characters.GetById (idFriendly)->GetPosition (),
             HexCoord (-8, 10));
  for (unsigned i = 0; i < 10; ++i)
    UpdateState ("[]");
  EXPECT_EQ (characters.GetById (idTarget)->GetPosition (), HexCoord (11, -10));
  EXPECT_EQ (characters.GetById (idFriendly)->GetPosition (),
             HexCoord (10, 10));
  UpdateState ("[]");
  EXPECT_EQ (characters.GetById (idTarget)->GetPosition (), HexCoord (13, -10));
  for (unsigned i = 0; i < 4; ++i)
    UpdateState ("[]");
  EXPECT_EQ (characters.GetById (idTarget)->GetPosition (), HexCoord (20, -10));
}

TEST_F (PXLogicTests, ProspectingBeforeMovement)
{
  /* This should test that prospecting is started before processing
     movement.  In other words, if a character is about to move to the
     next region when a "prospect" command hits, then prospecting should
     be started at the "old" region.  For this, we need two coordinates
     next to each other but in different regions.  */
  HexCoord pos1, pos2;
  RegionMap::IdT region1, region2;
  for (HexCoord::IntT x = 0; ; ++x)
    {
      pos1 = HexCoord (x, 0);
      region1 = ctx.Map ().Regions ().GetRegionId (pos1);
      pos2 = HexCoord (x + 1, 0);
      region2 = ctx.Map ().Regions ().GetRegionId (pos2);
      if (region1 != region2)
        break;
    }
  CHECK_NE (region1, region2);
  CHECK_EQ (HexCoord::DistanceL1 (pos1, pos2), 1);
  LOG (INFO)
      << "Neighbouring coordinates " << pos1 << " and " << pos2
      << " are in differing regions " << region1 << " and " << region2;

  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 1);
  c->SetPosition (pos1);
  c->MutableVolatileMv ().set_partial_step (1000);
  auto& pb = c->MutableProto ();
  pb.mutable_combat_data ();
  *pb.mutable_movement ()->add_waypoints () = CoordToProto (pos2);
  pb.set_prospecting_blocks (10);
  c.reset ();

  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 1, "prospect": {}}}
    }
  ])");

  c = characters.GetById (1);
  EXPECT_EQ (c->GetPosition (), pos1);
  EXPECT_TRUE (c->IsBusy ());

  auto r = regions.GetById (region1);
  EXPECT_EQ (r->GetProto ().prospecting_character (), 1);
  r = regions.GetById (region2);
  EXPECT_FALSE (r->GetProto ().has_prospecting_character ());
}

TEST_F (PXLogicTests, ProspectingUserKilled)
{
  const HexCoord pos(5, 5);
  const auto region = ctx.Map ().Regions ().GetRegionId (pos);

  /* Set up characters such that one is killing the other on the next round.  */
  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 1);
  c->SetPosition (pos);
  c->MutableProto ().set_prospecting_blocks (10);
  AddUnityAttack (*c, 1);
  c.reset ();

  c = CreateCharacter ("andy", Faction::GREEN);
  ASSERT_EQ (c->GetId (), 2);
  c->SetPosition (pos);
  c->MutableProto ().set_prospecting_blocks (10);
  c->MutableProto ().mutable_combat_data ();
  auto& regen = c->MutableRegenData ();
  regen.mutable_max_hp ()->set_shield (100);
  c->MutableHP ().set_shield (1);
  c->MutableHP ().set_armour (0);
  c.reset ();

  /* Progress one round forward to target and also start prospecting
     with the character that will be killed.  */
  UpdateState (R"([
    {
      "name": "andy",
      "move": {"c": {"id": 2, "prospect": {}}}
    }
  ])");

  c = characters.GetById (2);
  EXPECT_TRUE (c->IsBusy ());
  auto op = ongoings.GetById (c->GetProto ().ongoing ());
  ASSERT_NE (op, nullptr);
  EXPECT_TRUE (op->GetProto ().has_prospection ());

  /* Make sure that the prospecting operation would be finished on the next
     step (but it won't be as the character is killed).  */
  SetHeight (op->GetHeight ());
  op.reset ();
  c.reset ();

  auto r = regions.GetById (region);
  EXPECT_EQ (r->GetProto ().prospecting_character (), 2);

  /* Process another round, where the prospecting character is killed.  Thus
     the other is able to start prospecting at the same spot.  */
  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 1, "prospect": {}}}
    }
  ])");

  EXPECT_TRUE (characters.GetById (2) == nullptr);

  c = characters.GetById (1);
  EXPECT_TRUE (c->IsBusy ());

  r = regions.GetById (region);
  EXPECT_EQ (r->GetProto ().prospecting_character (), 1);
  EXPECT_FALSE (r->GetProto ().has_prospection ());
}

TEST_F (PXLogicTests, FinishingProspecting)
{
  const HexCoord pos(5, 5);
  const auto region = ctx.Map ().Regions ().GetRegionId (pos);

  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 1);
  c->SetPosition (pos);
  c->MutableProto ().set_prospecting_blocks (10);
  c->MutableProto ().mutable_combat_data ();
  c->MutableProto ().set_speed (1000);
  c.reset ();

  /* Start prospecting with that character.  */
  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 1, "prospect": {}}}
    }
  ])");

  c = characters.GetById (1);
  EXPECT_TRUE (c->IsBusy ());
  auto op = ongoings.GetById (c->GetProto ().ongoing ());
  EXPECT_TRUE (op->GetProto ().has_prospection ());

  /* Set context height so that the next block finishes prospecting.  */
  SetHeight (op->GetHeight ());
  op.reset ();
  c.reset ();

  auto r = regions.GetById (region);
  EXPECT_EQ (r->GetProto ().prospecting_character (), 1);
  EXPECT_FALSE (r->GetProto ().has_prospection ());

  /* Process the next block which finishes prospecting.  We should be able
     to do a movement command right away as well, since the busy state is
     processed before the moves.  */
  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 1, "wp": )" + WpStr ({HexCoord (0, 5)}) + R"(}}
    }
  ])");

  c = characters.GetById (1);
  EXPECT_FALSE (c->IsBusy ());
  EXPECT_TRUE (c->GetProto ().has_movement ());

  r = regions.GetById (region);
  EXPECT_FALSE (r->GetProto ().has_prospecting_character ());
  EXPECT_EQ (r->GetProto ().prospection ().name (), "domob");
}

TEST_F (PXLogicTests, MiningRightAfterProspecting)
{
  const HexCoord pos(5, 5);
  const auto region = ctx.Map ().Regions ().GetRegionId (pos);

  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 1);
  c->SetPosition (pos);
  c->MutableProto ().mutable_combat_data ();
  c->MutableProto ().mutable_mining ()->mutable_rate ()->set_min (1);
  c->MutableProto ().mutable_mining ()->mutable_rate ()->set_max (1);
  c->MutableProto ().set_prospecting_blocks (10);
  c->MutableProto ().set_cargo_space (100);
  c.reset ();

  /* Prospect the region with the character.  */
  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 1, "prospect": {}}}
    }
  ])");
  c = characters.GetById (1);
  auto op = ongoings.GetById (c->GetProto ().ongoing ());
  SetHeight (op->GetHeight ());
  op.reset ();
  c.reset ();

  /* In the next block, prospecting will be finished.  We can already start
     mining the now-prospected region immediately.  */
  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 1, "mine": {}}}
    }
  ])");

  auto r = regions.GetById (region);
  const std::string type = r->GetProto ().prospection ().resource ();
  LOG (INFO) << "Resource found: " << type;

  c = characters.GetById (1);
  EXPECT_FALSE (c->IsBusy ());
  EXPECT_TRUE (c->GetProto ().mining ().active ());
  EXPECT_EQ (c->GetInventory ().GetFungibleCount (type), 1);
}

TEST_F (PXLogicTests, MiningAndDropping)
{
  const HexCoord pos(5, 5);
  const auto region = ctx.Map ().Regions ().GetRegionId (pos);

  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 1);
  c->SetPosition (pos);
  c->MutableProto ().mutable_combat_data ();
  c->MutableProto ().mutable_mining ()->mutable_rate ()->set_min (10);
  c->MutableProto ().mutable_mining ()->mutable_rate ()->set_max (10);
  c->MutableProto ().mutable_mining ()->set_active (true);
  c->MutableProto ().set_cargo_space (1000);
  c->GetInventory ().SetFungibleCount ("foo", 95);
  c.reset ();

  auto r = regions.GetById (region);
  r->MutableProto ().mutable_prospection ()->set_resource ("foo");
  r->SetResourceLeft (1000);
  r.reset ();

  /* Processing one block will mine some more, filling up the inventory.  */
  UpdateState ("[]");
  c = characters.GetById (1);
  EXPECT_TRUE (c->GetProto ().mining ().active ());
  EXPECT_EQ (c->GetInventory ().GetFungibleCount ("foo"), 100);
  c.reset ();
  EXPECT_EQ (regions.GetById (region)->GetResourceLeft (), 995);

  /* In the next block, drop loot.  This should take effect before mining,
     so that we will be able to mine some more afterwards.  */
  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 1, "drop": {"f": {"foo": 3}}}}
    }
  ])");
  c = characters.GetById (1);
  EXPECT_TRUE (c->GetProto ().mining ().active ());
  EXPECT_EQ (c->GetInventory ().GetFungibleCount ("foo"), 100);
  c.reset ();
  EXPECT_EQ (regions.GetById (region)->GetResourceLeft (), 992);

  /* One more block where we won't pick up anything, so we will stop mining.  */
  UpdateState ("[]");
  c = characters.GetById (1);
  EXPECT_FALSE (c->GetProto ().mining ().active ());
  EXPECT_EQ (c->GetInventory ().GetFungibleCount ("foo"), 100);
  c.reset ();
  EXPECT_EQ (regions.GetById (region)->GetResourceLeft (), 992);
}

TEST_F (PXLogicTests, MiningWhenReprospected)
{
  const HexCoord pos(5, 5);
  const auto region = ctx.Map ().Regions ().GetRegionId (pos);

  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 1);
  c->SetPosition (pos);
  c->MutableProto ().mutable_combat_data ();
  c->MutableProto ().mutable_mining ()->mutable_rate ()->set_min (1);
  c->MutableProto ().mutable_mining ()->mutable_rate ()->set_max (1);
  c->MutableProto ().mutable_mining ()->set_active (true);
  c->MutableProto ().set_prospecting_blocks (10);
  c->MutableProto ().set_cargo_space (1000);
  c.reset ();

  auto r = regions.GetById (region);
  r->MutableProto ().mutable_prospection ()->set_height (1);
  r->MutableProto ().mutable_prospection ()->set_resource ("foo");
  r->SetResourceLeft (1);
  r.reset ();

  /* When we reprospect the region while still mining, this should just stop
     mining gracefully.  We can only reprospect after using up the resources,
     which means that we need to mine for one turn before.  */
  UpdateState ("[]");
  auto data = BuildBlockData (ParseJson (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 1, "prospect": {}}}
    }
  ])"));
  data["block"]["height"] = 200;
  UpdateStateWithData (data);

  c = characters.GetById (1);
  EXPECT_TRUE (c->IsBusy ());
  EXPECT_FALSE (c->GetProto ().mining ().active ());
  c.reset ();
  EXPECT_FALSE (regions.GetById (region)->GetProto ().has_prospection ());
}

TEST_F (PXLogicTests, EnterBuildingAfterMovesAndMovement)
{
  auto b = CreateBuilding ("checkmark", "", Faction::ANCIENT);
  ASSERT_EQ (b->GetId (), 1);
  b->SetCentre (HexCoord (0, 0));
  b.reset ();

  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 2);
  c->SetPosition (HexCoord (6, 0));
  c->MutableProto ().set_speed (1'000);
  auto* mv = c->MutableProto ().mutable_movement ();
  *mv->add_waypoints () = CoordToProto (HexCoord (5, 0));
  c.reset ();

  /* Setting the "enter building" intent and moving into range both
     happen in this very update.  */
  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 2, "eb": 1}}
    }
  ])");

  c = characters.GetById (2);
  ASSERT_TRUE (c->IsInBuilding ());
  EXPECT_EQ (c->GetBuildingId (), 1);
}

TEST_F (PXLogicTests, EnterBuildingDelayed)
{
  auto b = CreateBuilding ("checkmark", "", Faction::ANCIENT);
  ASSERT_EQ (b->GetId (), 1);
  b->SetCentre (HexCoord (0, 0));
  b.reset ();

  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 2);
  c->SetPosition (HexCoord (10, 0));
  c->SetEnterBuilding (1);
  c->MutableProto ().set_speed (1'000);
  auto* mv = c->MutableProto ().mutable_movement ();
  *mv->add_waypoints () = CoordToProto (HexCoord (5, 0));
  c.reset ();

  /* We will be in range exactly after 5 updates.  */
  for (unsigned i = 0; i < 5; ++i)
    {
      EXPECT_FALSE (characters.GetById (2)->IsInBuilding ());
      UpdateState ("[]");
    }

  c = characters.GetById (2);
  ASSERT_TRUE (c->IsInBuilding ());
  EXPECT_EQ (c->GetBuildingId (), 1);
}

TEST_F (PXLogicTests, EnterBuildingAndTargetFinding)
{
  auto b = CreateBuilding ("checkmark", "", Faction::ANCIENT);
  ASSERT_EQ (b->GetId (), 1);
  b->SetCentre (HexCoord (0, 0));
  b.reset ();

  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 2);
  c->MutableHP ().set_armour (100);
  c->SetPosition (HexCoord (3, 0));
  AddUnityAttack (*c, 10);
  c.reset ();

  c = CreateCharacter ("andy", Faction::BLUE);
  ASSERT_EQ (c->GetId (), 3);
  c->MutableHP ().set_armour (100);
  c->SetPosition (HexCoord (0, 3));
  AddUnityAttack (*c, 10);
  c.reset ();

  /* Both characters will target and attack each other.  */
  UpdateState ("[]");

  EXPECT_EQ (characters.GetById (2)->GetTarget ().id (), 3);
  EXPECT_EQ (characters.GetById (3)->GetTarget ().id (), 2);

  /* Now the "domob" character will enter the building, and neither will
     target the other any more.  */
  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 2, "eb": 1}}
    }
  ])");

  EXPECT_FALSE (characters.GetById (2)->HasTarget ());
  EXPECT_FALSE (characters.GetById (3)->HasTarget ());
}

TEST_F (PXLogicTests, EnterAndExitBuildingWhenOutside)
{
  auto b = CreateBuilding ("checkmark", "", Faction::ANCIENT);
  ASSERT_EQ (b->GetId (), 1);
  b->SetCentre (HexCoord (0, 0));
  b.reset ();

  auto c = CreateCharacter ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 2);
  c->SetPosition (HexCoord (5, 0));
  c.reset ();

  /* Entering and exiting a building in the same move will only enter,
     as the exit is invalid until the enter intents are actually processed
     (which is after processing moves).  */
  UpdateState (R"([
    {
      "name": "domob",
      "move": {"c": {"id": 2, "eb": 1, "xb": {}}}
    }
  ])");

  c = characters.GetById (2);
  ASSERT_TRUE (c->IsInBuilding ());
  EXPECT_EQ (c->GetBuildingId (), 1);
}

TEST_F (PXLogicTests, BuildingUpdateVsOperations)
{
  accounts.CreateNew ("owner")->SetFaction (Faction::RED);
  accounts.CreateNew ("buyer")->SetFaction (Faction::RED);
  accounts.CreateNew ("seller")->SetFaction (Faction::RED);
  accounts.CreateNew ("user")->SetFaction (Faction::RED);

  auto b = CreateBuilding ("itemmaker", "owner", Faction::RED);
  ASSERT_EQ (b->GetId (), 1);
  b.reset ();

  /* We want to execute both DEX trades and services inside the building
     to verify the fees charged for them (based on how/when fee updates
     of the building owner take effect).  Set up all we need so that
     we can do those operations.  */
  accounts.GetByName ("buyer")->AddBalance (1'000'000);
  inv.Get (1, "seller")->GetInventory ().AddFungibleCount ("foo", 100);
  accounts.GetByName ("user")->AddBalance (1'000);
  inv.Get (1, "user")->GetInventory ().AddFungibleCount ("sword bpo", 1);
  inv.Get (1, "user")->GetInventory ().AddFungibleCount ("zerospace", 10);
  UpdateState (R"([
    {
      "name": "buyer",
      "move": {"x": [{"b": 1, "i": "foo", "n": 100, "bp": 100}]}
    }
  ])");

  /* Increase fees in the building on each block.  When they hit 100% for
     services and 100 bps (1%) for DEX trades, actually perform the actions.  */
  for (int i = 1; i <= 200; ++i)
    {
      ctx.SetHeight (ctx.Height () + 1);

      auto moves = ParseJson (R"([
        {
          "name": "owner",
          "move": {"b": {"id": 1}}
        }
      ])");
      auto& bMove = moves[0]["move"]["b"];
      bMove["sf"] = IntToJson (i);
      bMove["xf"] = IntToJson (i);

      /* The ongoing operations are processed before moves.  So if the
         building update had a delay of only one block, the previous block's
         update would be active now (i.e. the fee would be i-1).  The real
         delay on regtest is 10 blocks, so the fee in effect is i-10.  */
      if (i == 110)
        {
          moves.append (ParseJson (R"({
            "name": "seller",
            "move": {"x": [{"b": 1, "i": "foo", "n": 100, "ap": 100}]}
          })"));
          moves.append (ParseJson (R"({
            "name": "user",
            "move": {"s": [{"b": 1, "t": "bld", "i": "sword bpo", "n": 1}]}
          })"));
        }

      UpdateStateJson (moves);
    }

  /* Both the trade and item construction should have been executed with
     fee 100 (100% for construction, 1% for the trade).  The trade nets
     10k Cubits in total, so 1k Cubits for the base fee and 100 Cubits for
     the building owner.  The item construction costs 100 Cubits base fee,
     so also 100 Cubits in fees for the building.  */
  EXPECT_EQ (accounts.GetByName ("owner")->GetBalance (), 200);
  EXPECT_EQ (accounts.GetByName ("seller")->GetBalance (), 10'000 - 1'100);
  EXPECT_EQ (accounts.GetByName ("buyer")->GetBalance (), 1'000'000 - 10'000);
}

/* ************************************************************************** */

using ValidateStateTests = PXLogicTests;

TEST_F (ValidateStateTests, AncientAccountFaction)
{
  accounts.CreateNew ("domob")->SetFaction (Faction::ANCIENT);
  EXPECT_DEATH (ValidateState (), "has invalid faction");
}

TEST_F (ValidateStateTests, CharacterFactions)
{
  auto c = characters.CreateNew ("domob", Faction::RED);
  const auto id = c->GetId ();
  c.reset ();
  EXPECT_DEATH (ValidateState (), "owned by uninitialised account");

  accounts.CreateNew ("domob")->SetFaction (Faction::GREEN);
  EXPECT_DEATH (ValidateState (), "Faction mismatch");

  accounts.CreateNew ("andy")->SetFaction (Faction::RED);
  characters.GetById (id)->SetOwner ("andy");
  ValidateState ();
}

TEST_F (ValidateStateTests, BuildingOwnerFactions)
{
  CreateBuilding ("ancient1", "", Faction::ANCIENT);

  auto h = CreateBuilding ("checkmark", "domob", Faction::RED);
  const auto id = h->GetId ();
  h.reset ();
  EXPECT_DEATH (ValidateState (), "owned by uninitialised account");

  accounts.CreateNew ("domob")->SetFaction (Faction::GREEN);
  EXPECT_DEATH (ValidateState (), "Faction mismatch");

  accounts.CreateNew ("andy")->SetFaction (Faction::RED);
  buildings.GetById (id)->SetOwner ("andy");
  ValidateState ();
}

TEST_F (ValidateStateTests, BuildingAgeData)
{
  auto b = buildings.CreateNew ("checkmark", "", Faction::ANCIENT);
  const auto id = b->GetId ();
  b->MutableProto ().set_foundation (true);
  b.reset ();
  EXPECT_DEATH (ValidateState (), "has no founded height");

  ctx.SetHeight (9);
  buildings.GetById (id)->MutableProto ().mutable_age_data ()
      ->set_founded_height (10);
  EXPECT_DEATH (ValidateState (), "founded in the future");

  ctx.SetHeight (10);
  ValidateState ();

  buildings.GetById (id)->MutableProto ().mutable_age_data ()
      ->set_finished_height (10);
  EXPECT_DEATH (ValidateState (), "has already finished height");

  buildings.GetById (id)->MutableProto ().set_foundation (false);
  ValidateState ();

  buildings.GetById (id)->MutableProto ().mutable_age_data ()
      ->clear_finished_height ();
  EXPECT_DEATH (ValidateState (), "has no finished height");

  buildings.GetById (id)->MutableProto ().mutable_age_data ()
      ->set_finished_height (9);
  EXPECT_DEATH (ValidateState (), "was finished before being founded");

  buildings.GetById (id)->MutableProto ().mutable_age_data ()
      ->set_finished_height (11);
  EXPECT_DEATH (ValidateState (), "finished in the future");

  ctx.SetHeight (11);
  ValidateState ();
}

TEST_F (ValidateStateTests, BuildingConstructionFaction)
{
  accounts.CreateNew ("red")->SetFaction (Faction::RED);
  accounts.CreateNew ("green")->SetFaction (Faction::GREEN);

  CreateBuilding ("checkmark", "", Faction::ANCIENT);
  CreateBuilding ("checkmark", "red", Faction::RED);
  CreateBuilding ("r test", "red", Faction::RED);
  CreateBuilding ("g test", "green", Faction::GREEN);
  ValidateState ();

  auto id = CreateBuilding ("r test", "green", Faction::GREEN)->GetId ();
  EXPECT_DEATH (ValidateState (), "base data requires faction");
  buildings.DeleteById (id);

  id = CreateBuilding ("r test", "", Faction::ANCIENT)->GetId ();
  EXPECT_DEATH (ValidateState (), "base data requires faction");
  buildings.DeleteById (id);

  ValidateState ();
}

TEST_F (ValidateStateTests, CharacterLimit)
{
  accounts.CreateNew ("domob")->SetFaction (Faction::RED);
  accounts.CreateNew ("andy")->SetFaction (Faction::GREEN);
  for (unsigned i = 0; i < ctx.RoConfig ()->params ().character_limit (); ++i)
    {
      characters.CreateNew ("domob", Faction::RED);
      characters.CreateNew ("andy", Faction::GREEN);
    }

  ValidateState ();

  characters.CreateNew ("domob", Faction::RED);
  EXPECT_DEATH (ValidateState (), "Account domob has too many");
}

TEST_F (ValidateStateTests, CharactersInBuildings)
{
  accounts.CreateNew ("domob")->SetFaction (Faction::RED);
  accounts.CreateNew ("andy")->SetFaction (Faction::BLUE);

  const auto idAncient
      = CreateBuilding ("checkmark", "", Faction::ANCIENT)->GetId ();
  const auto idOk
      = CreateBuilding ("checkmark", "domob", Faction::RED)->GetId ();
  const auto idWrong
      = CreateBuilding ("checkmark", "andy", Faction::BLUE)->GetId ();

  db.SetNextId (10);
  ASSERT_EQ (characters.CreateNew ("domob", Faction::RED)->GetId (), 10);

  characters.GetById (10)->SetBuildingId (idAncient);
  ValidateState ();

  characters.GetById (10)->SetBuildingId (idOk);
  ValidateState ();

  characters.GetById (10)->SetBuildingId (idWrong);
  EXPECT_DEATH (ValidateState (), "of opposing faction");

  characters.GetById (10)->SetBuildingId (12345);
  EXPECT_DEATH (ValidateState (), "is in non-existant building");
}

TEST_F (ValidateStateTests, CharacterCargoSpace)
{
  accounts.CreateNew ("domob")->SetFaction (Faction::RED);
  auto c = characters.CreateNew ("domob", Faction::RED);
  const auto id = c->GetId ();
  c->MutableProto ().set_cargo_space (19);
  c->GetInventory ().AddFungibleCount ("foo", 2);
  c.reset ();

  EXPECT_DEATH (ValidateState (), "exceeds cargo limit");
  characters.GetById (id)->MutableProto ().set_cargo_space (20);
  ValidateState ();
}

TEST_F (ValidateStateTests, BuildingInventories)
{
  db.SetNextId (10);
  accounts.CreateNew ("domob")->SetFaction (Faction::RED);
  CreateBuilding ("checkmark", "", Faction::ANCIENT);

  inv.Get (10, "andy")->GetInventory ().SetFungibleCount ("foo", 1);
  EXPECT_DEATH (ValidateState (), "non-existant account");
  accounts.CreateNew ("andy")->SetFaction (Faction::GREEN);
  ValidateState ();

  inv.Get (11, "domob")->GetInventory ().SetFungibleCount ("foo", 1);
  EXPECT_DEATH (ValidateState (), "non-existant building");
  CreateBuilding ("checkmark", "", Faction::ANCIENT);
  ValidateState ();

  auto b = buildings.GetById (10);
  b->MutableProto ().set_foundation (true);
  b->MutableProto ().mutable_age_data ()->clear_finished_height ();
  b.reset ();
  EXPECT_DEATH (ValidateState (), "in foundation");

  b = buildings.GetById (10);
  b->MutableProto ().set_foundation (false);
  b->MutableProto ().mutable_age_data ()->set_finished_height (0);
  b.reset ();
  ValidateState ();

  b = CreateBuilding ("checkmark", "domob", Faction::RED);
  ASSERT_EQ (b->GetId (), 12);
  b->MutableProto ().mutable_construction_inventory ();
  b.reset ();
  EXPECT_DEATH (ValidateState (), "has construction inventory");

  b = buildings.GetById (12);
  b->MutableProto ().set_foundation (true);
  b->MutableProto ().mutable_age_data ()->clear_finished_height ();
  b.reset ();
  ValidateState ();
}

TEST_F (ValidateStateTests, OngoingsToCharacterLink)
{
  accounts.CreateNew ("domob")->SetFaction (Faction::RED);
  db.SetNextId (101);

  auto op = ongoings.CreateNew (1);
  op->SetCharacterId (102);
  op.reset ();
  EXPECT_DEATH (ValidateState (), "refers to non-existing character");

  characters.CreateNew ("domob", Faction::RED);
  EXPECT_DEATH (ValidateState (), "does not refer back to ongoing");

  auto c = characters.GetById (102);
  c->MutableProto ().set_ongoing (101);
  c.reset ();
  ValidateState ();
}

TEST_F (ValidateStateTests, CharacterToOngoingsLink)
{
  accounts.CreateNew ("domob")->SetFaction (Faction::RED);
  db.SetNextId (101);

  auto c = characters.CreateNew ("domob", Faction::RED);
  c->MutableProto ().set_ongoing (102);
  c.reset ();
  EXPECT_DEATH (ValidateState (), "has non-existing ongoing");

  ongoings.CreateNew (1);
  EXPECT_DEATH (ValidateState (), "does not refer back to character");

  auto op = ongoings.GetById (102);
  op->SetCharacterId (101);
  op.reset ();
  ValidateState ();
}

TEST_F (ValidateStateTests, OngoingsToBuildingLink)
{
  accounts.CreateNew ("domob")->SetFaction (Faction::RED);
  db.SetNextId (101);

  auto op = ongoings.CreateNew (1);
  op->SetBuildingId (102);
  op.reset ();
  EXPECT_DEATH (ValidateState (), "refers to non-existing building");

  CreateBuilding ("checkmark", "domob", Faction::RED);
  /* If the ongoing operation is not a building construction, it is fine
     that the building does not refer to it (in practice, this might be
     blueprint copies or item constructions going on inside the building).  */
  ValidateState ();

  ongoings.GetById (101)->MutableProto ().mutable_building_construction ();
  EXPECT_DEATH (ValidateState (), "does not refer back to ongoing");

  auto b = buildings.GetById (102);
  b->MutableProto ().set_ongoing_construction (101);
  b.reset ();
  ValidateState ();
}

TEST_F (ValidateStateTests, BuildingToOngoingsLink)
{
  accounts.CreateNew ("domob")->SetFaction (Faction::RED);
  db.SetNextId (101);

  auto b = CreateBuilding ("checkmark", "domob", Faction::RED);
  b->MutableProto ().set_ongoing_construction (102);
  b.reset ();
  EXPECT_DEATH (ValidateState (), "has non-existing ongoing");

  ongoings.CreateNew (1)->MutableProto ().mutable_building_construction ();
  EXPECT_DEATH (ValidateState (), "does not refer back to building");

  ongoings.GetById (102)->SetBuildingId (101);
  ValidateState ();

  ongoings.GetById (102)->MutableProto ().mutable_blueprint_copy ();
  EXPECT_DEATH (ValidateState (), "that is not a building construction");
}

TEST_F (ValidateStateTests, DexOrderLinks)
{
  accounts.CreateNew ("domob");
  db.SetNextId (100);
  CreateBuilding ("checkmark", "", Faction::ANCIENT);

  db.SetNextId (201);
  orders.CreateNew (100, "domob", DexOrder::Type::ASK, "foo", 1, 1);
  ValidateState ();

  orders.CreateNew (100, "invalid", DexOrder::Type::ASK, "foo", 1, 1);
  EXPECT_DEATH (ValidateState (), "non-existing account");
  orders.GetById (202)->Delete ();

  orders.CreateNew (101, "domob", DexOrder::Type::ASK, "foo", 1, 1);
  EXPECT_DEATH (ValidateState (), "non-existing building");
  orders.GetById (203)->Delete ();

  auto b = buildings.GetById (100);
  b->MutableProto ().set_foundation (true);
  b->MutableProto ().mutable_age_data ()->clear_finished_height ();
  b.reset ();
  EXPECT_DEATH (ValidateState (), "is in foundation");
}

/* ************************************************************************** */

} // anonymous namespace
} // namespace pxd
