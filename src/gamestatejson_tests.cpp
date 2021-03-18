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

#include "gamestatejson.hpp"

#include "protoutils.hpp"
#include "services.hpp"
#include "testutils.hpp"

#include "database/account.hpp"
#include "database/building.hpp"
#include "database/character.hpp"
#include "database/dbtest.hpp"
#include "database/dex.hpp"
#include "database/inventory.hpp"
#include "database/itemcounts.hpp"
#include "database/moneysupply.hpp"
#include "database/ongoing.hpp"
#include "database/region.hpp"
#include "proto/character.pb.h"
#include "proto/region.pb.h"

#include <gtest/gtest.h>

#include <json/json.h>

#include <string>

namespace pxd
{
namespace
{

/* ************************************************************************** */

class GameStateJsonTests : public DBTestWithSchema
{

protected:

  ContextForTesting ctx;

  /** GameStateJson instance used in testing.  */
  GameStateJson converter;

  GameStateJsonTests ()
    : converter(db, ctx)
  {}

  /**
   * Expects that the current state matches the given one, after parsing
   * the expected state's string as JSON.  Furthermore, the expected value
   * is assumed to be *partial* -- keys that are not present in the expected
   * value may be present with any value in the actual object.  If a key is
   * present in expected but has value null, then it must not be present
   * in the actual data, though.
   */
  void
  ExpectStateJson (const std::string& expectedStr)
  {
    const Json::Value actual = converter.FullState ();
    VLOG (1) << "Actual JSON for the game state:\n" << actual;
    ASSERT_TRUE (PartialJsonEqual (actual, ParseJson (expectedStr)));
  }

};

/* ************************************************************************** */

class CharacterJsonTests : public GameStateJsonTests
{

protected:

  CharacterTable tbl;

  CharacterJsonTests ()
    : tbl(db)
  {}

};

TEST_F (CharacterJsonTests, Basic)
{
  auto c = tbl.CreateNew ("domob", Faction::RED);
  c->SetPosition (HexCoord (-5, 2));
  c->MutableProto ().set_speed (750);
  c.reset ();

  tbl.CreateNew ("andy", Faction::GREEN)->SetBuildingId (100);

  ExpectStateJson (R"({
    "characters":
      [
        {
          "id": 1, "owner": "domob", "faction": "r",
          "speed": 750,
          "inbuilding": null,
          "position": {"x": -5, "y": 2}
        },
        {
          "id": 2, "owner": "andy", "faction": "g",
          "inbuilding": 100,
          "position": null
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, EnterBuilding)
{
  tbl.CreateNew ("domob", Faction::RED);
  tbl.CreateNew ("andy", Faction::BLUE)->SetEnterBuilding (5);

  ExpectStateJson (R"({
    "characters":
      [
        {
          "id": 1, "owner": "domob", "faction": "r",
          "enterbuilding": null
        },
        {
          "id": 2, "owner": "andy", "faction": "b",
          "enterbuilding": 5
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, ChosenSpeed)
{
  auto c = tbl.CreateNew ("domob", Faction::RED);
  c->MutableProto ().mutable_movement ()->set_chosen_speed (1234);
  c.reset ();

  ExpectStateJson (R"({
    "characters":
      [
        {
          "movement":
            {
              "chosenspeed": 1234
            }
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, Waypoints)
{
  auto c = tbl.CreateNew ("domob", Faction::RED);
  c->MutableVolatileMv ().set_partial_step (5);
  c->MutableVolatileMv ().set_blocked_turns (3);
  auto* wp = c->MutableProto ().mutable_movement ()->mutable_waypoints ();
  *wp->Add () = CoordToProto (HexCoord (-3, 0));
  *wp->Add () = CoordToProto (HexCoord (0, 42));
  c.reset ();

  ExpectStateJson (R"({
    "characters":
      [
        {
          "movement":
            {
              "partialstep": 5,
              "blockedturns": 3,
              "waypoints": [{"x": -3, "y": 0}, {"x": 0, "y": 42}]
            }
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, VehicleAndFitments)
{
  auto c = tbl.CreateNew ("domob", Faction::RED);
  c->MutableProto ().set_vehicle ("rv st");
  c->MutableProto ().add_fitments ("turbo");
  c->MutableProto ().add_fitments ("bomb");
  c.reset ();

  tbl.CreateNew ("andy", Faction::GREEN);

  ExpectStateJson (R"({
    "characters":
      [
        {
          "vehicle": "rv st",
          "fitments": ["turbo", "bomb"]
        },
        {
          "fitments": []
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, Target)
{
  auto c = tbl.CreateNew ("domob", Faction::RED);
  proto::TargetId t;
  t.set_id (5);
  t.set_type (proto::TargetId::TYPE_CHARACTER);
  c->SetTarget (t);
  c.reset ();

  c = tbl.CreateNew ("domob", Faction::GREEN);
  t.set_id (42);
  t.set_type (proto::TargetId::TYPE_BUILDING);
  c->SetTarget (t);
  c.reset ();

  tbl.CreateNew ("domob", Faction::BLUE);

  ExpectStateJson (R"({
    "characters":
      [
        {"faction": "r", "combat": {"target": {"id": 5, "type": "character"}}},
        {"faction": "g", "combat": {"target": {"id": 42, "type": "building"}}},
        {"faction": "b", "combat": {"target": null}}
      ]
  })");
}

TEST_F (CharacterJsonTests, Attacks)
{
  auto c = tbl.CreateNew ("domob", Faction::RED);
  auto* cd = c->MutableProto ().mutable_combat_data ();

  auto* attack = cd->add_attacks ();
  attack->set_range (5);
  attack->mutable_damage ()->set_min (2);
  attack->mutable_damage ()->set_max (10);

  attack = cd->add_attacks ();
  attack->set_area (1);
  attack->mutable_damage ()->set_min (0);
  attack->mutable_damage ()->set_max (1);

  attack = cd->add_attacks ();
  attack->set_area (10);
  attack->set_friendlies (true);

  c.reset ();

  ExpectStateJson (R"({
    "characters":
      [
        {
          "combat":
            {
              "attacks":
                [
                  {
                    "range": 5,
                    "damage": {"min": 2, "max": 10}
                  },
                  {
                    "area": 1,
                    "friendlies": null,
                    "damage": {"min": 0, "max": 1}
                  },
                  {
                    "area": 10,
                    "friendlies": true
                  }
                ]
            }
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, HP)
{
  auto c = tbl.CreateNew ("domob", Faction::RED);
  c->MutableHP ().set_armour (42);
  c->MutableHP ().set_shield (5);
  c->MutableHP ().mutable_mhp ()->set_shield (1);
  auto& regen = c->MutableRegenData ();
  regen.mutable_max_hp ()->set_armour (100);
  regen.mutable_max_hp ()->set_shield (10);
  regen.mutable_regeneration_mhp ()->set_shield (1'001);
  regen.mutable_regeneration_mhp ()->set_armour (42);
  c.reset ();

  ExpectStateJson (R"({
    "characters":
      [
        {
          "combat":
            {
              "hp":
                {
                  "max": {"armour": 100, "shield": 10},
                  "current": {"armour": 42, "shield": 5.001},
                  "regeneration": {"shield": 1.001, "armour": 0.042}
                }
            }
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, Inventory)
{
  auto h = tbl.CreateNew ("domob", Faction::RED);
  h->MutableProto ().set_cargo_space (1000);
  h->GetInventory ().SetFungibleCount ("foo", 5);
  h->GetInventory ().SetFungibleCount ("bar", 10);
  h.reset ();

  ExpectStateJson (R"({
    "characters":
      [
        {
          "inventory":
            {
              "fungible":
                {
                  "foo": 5,
                  "bar": 10
                }
            }
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, CargoSpace)
{
  auto h = tbl.CreateNew ("domob", Faction::RED);
  h->MutableProto ().set_cargo_space (1000);
  h->GetInventory ().SetFungibleCount ("foo", 35);
  h.reset ();

  ExpectStateJson (R"({
    "characters":
      [
        {
          "cargospace":
            {
              "total": 1000,
              "used": 350,
              "free": 650
            }
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, Mining)
{
  const HexCoord pos(10, -5);
  ASSERT_EQ (ctx.Map ().Regions ().GetRegionId (pos), 350146);

  tbl.CreateNew ("without mining", Faction::RED);

  auto h = tbl.CreateNew ("inactive mining", Faction::RED);
  h->MutableProto ().mutable_mining ()->mutable_rate ()->set_min (0);
  h->MutableProto ().mutable_mining ()->mutable_rate ()->set_max (5);
  h.reset ();

  h = tbl.CreateNew ("active mining", Faction::RED);
  h->SetPosition (pos);
  h->MutableProto ().mutable_mining ()->mutable_rate ()->set_min (10);
  h->MutableProto ().mutable_mining ()->mutable_rate ()->set_max (11);
  h->MutableProto ().mutable_mining ()->set_active (true);
  h.reset ();

  ExpectStateJson (R"({
    "characters":
      [
        {
          "owner": "without mining",
          "mining": null
        },
        {
          "owner": "inactive mining",
          "mining":
            {
              "rate":
                {
                  "min": 0,
                  "max": 5
                },
              "active": false,
              "region": null
            }
        },
        {
          "owner": "active mining",
          "mining":
            {
              "rate":
                {
                  "min": 10,
                  "max": 11
                },
              "active": true,
              "region": 350146
            }
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, ProspectingRate)
{
  tbl.CreateNew ("without prospecting", Faction::RED);

  auto c = tbl.CreateNew ("with prospecting", Faction::RED);
  c->MutableProto ().set_prospecting_blocks (42);
  c.reset ();

  ExpectStateJson (R"({
    "characters":
      [
        {
          "owner": "without prospecting",
          "prospectingblocks": null
        },
        {
          "owner": "with prospecting",
          "prospectingblocks": 42
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, MobileRefinery)
{
  tbl.CreateNew ("no refining", Faction::RED);

  auto c = tbl.CreateNew ("has refinery", Faction::RED);
  c->MutableProto ().mutable_refining ()->mutable_input ()->set_percent (100);
  c.reset ();

  ExpectStateJson (R"({
    "characters":
      [
        {
          "owner": "no refining",
          "refining": null
        },
        {
          "owner": "has refinery",
          "refining": {"inefficiency": 200}
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, DamageLists)
{
  const auto id1 = tbl.CreateNew ("domob", Faction::RED)->GetId ();
  const auto id2 = tbl.CreateNew ("domob", Faction::GREEN)->GetId ();
  ASSERT_EQ (id2, 2);

  DamageLists dl(db, 0);
  dl.AddEntry (id1, id2);

  ExpectStateJson (R"({
    "characters":
      [
        {
          "faction": "r",
          "combat":
            {
              "attackers": [2]
            }
        },
        {
          "faction": "g",
          "combat":
            {
              "attackers": null
            }
        }
      ]
  })");
}

TEST_F (CharacterJsonTests, Busy)
{
  auto c = tbl.CreateNew ("domob", Faction::RED);
  ASSERT_EQ (c->GetId (), 1);
  c->MutableProto ().set_ongoing (42);
  c.reset ();

  tbl.CreateNew ("notbusy", Faction::RED);

  ExpectStateJson (R"({
    "characters":
      [
        {
          "owner": "domob",
          "busy": 42
        },
        {
          "owner": "notbusy",
          "busy": null
        }
      ]
  })");
}

/* ************************************************************************** */

class AccountJsonTests : public GameStateJsonTests
{

protected:

  AccountsTable tbl;

  AccountJsonTests ()
    : tbl(db)
  {}

};

TEST_F (AccountJsonTests, KillsAndFame)
{
  auto a = tbl.CreateNew ("foo");
  a->SetFaction (Faction::RED);
  a->MutableProto ().set_kills (10);
  a.reset ();

  a = tbl.CreateNew ("bar");
  a->SetFaction (Faction::BLUE);
  a->MutableProto ().set_fame (42);
  a.reset ();

  ExpectStateJson (R"({
    "accounts":
      [
        {"name": "bar", "faction": "b", "kills": 0, "fame": 42},
        {"name": "foo", "faction": "r", "kills": 10, "fame": 100}
      ]
  })");
}

TEST_F (AccountJsonTests, UninitialisedBalance)
{
  tbl.CreateNew ("foo")->SetFaction (Faction::RED);

  auto a = tbl.CreateNew ("bar");
  a->MutableProto ().set_burnsale_balance (10);
  a->AddBalance (42);
  a.reset ();

  DexOrderTable orders(db);
  orders.CreateNew (5, "bar", DexOrder::Type::BID, "foo", 2, 3);

  ExpectStateJson (R"({
    "accounts":
      [
        {
          "name": "bar",
          "faction": null,
          "balance":
            {
              "available": 42,
              "reserved": 6,
              "total": 48
            },
          "minted": 10
        },
        {
          "name": "foo",
          "faction": "r",
          "balance":
            {
              "available": 0,
              "reserved": 0,
              "total": 0
            },
          "minted": 0
        }
      ]
  })");
}

TEST_F (AccountJsonTests, Skills)
{
  tbl.CreateNew ("uninit");

  auto a = tbl.CreateNew ("domob");
  a->SetFaction (Faction::RED);
  a->Skills ()[proto::SKILL_BUILDING].AddXp (10);
  a.reset ();

  /* We only check two skills:  One with data, and one without; the
     actual JSON will contain data for all skills, but we don't have to
     explicitly list all the other ones without data.  */

  ExpectStateJson (R"({
    "accounts":
      [
        {
          "name": "domob",
          "skills":
            {
              "building": {"xp": 10},
              "combat": {"xp": 0}
            }
        },
        {
          "name": "uninit",
          "skills": null
        }
      ]
  })");
}

/* ************************************************************************** */

class BuildingJsonTests : public GameStateJsonTests
{

protected:

  BuildingsTable tbl;
  BuildingInventoriesTable inv;

  BuildingJsonTests ()
    : tbl(db), inv(db)
  {}

};

TEST_F (BuildingJsonTests, Basic)
{
  auto h = tbl.CreateNew ("checkmark", "foo", Faction::RED);
  h->SetCentre (HexCoord (1, 2));
  h.reset ();

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "type": "checkmark",
          "owner": "foo",
          "faction": "r",
          "centre": {"x": 1, "y": 2},
          "rotationsteps": 0,
          "tiles":
            [
              {"x": 1, "y": 2},
              {"x": 2, "y": 2},
              {"x": 1, "y": 3},
              {"x": 1, "y": 4}
            ]
        }
      ]
  })");
}

TEST_F (BuildingJsonTests, Ancient)
{
  tbl.CreateNew ("checkmark", "", Faction::ANCIENT);

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "owner": null,
          "faction": "a"
        }
      ]
  })");
}

TEST_F (BuildingJsonTests, Foundation)
{
  tbl.CreateNew ("checkmark", "foo", Faction::RED);

  auto b = tbl.CreateNew ("checkmark", "foo", Faction::RED);
  b->MutableProto ().set_foundation (false);
  b.reset ();

  b = tbl.CreateNew ("checkmark", "foo", Faction::RED);
  b->MutableProto ().set_foundation (true);
  b->MutableProto ().mutable_construction_inventory ()
      ->mutable_fungible ()->insert ({"bar", 10});
  b.reset ();

  b = tbl.CreateNew ("checkmark", "foo", Faction::RED);
  b->MutableProto ().set_foundation (true);
  b->MutableProto ().set_ongoing_construction (42);
  b.reset ();

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "foundation": null,
          "construction": null,
          "inventories": {}
        },
        {
          "id": 2,
          "foundation": null,
          "construction": null
        },
        {
          "id": 3,
          "foundation": true,
          "construction":
            {
              "ongoing": null,
              "inventory": {"fungible": {"bar": 10}}
            },
          "inventories": null
        },
        {
          "id": 4,
          "foundation": true,
          "construction":
            {
              "ongoing": 42,
              "inventory": {"fungible": {}}
            }
        }
      ]
  })");
}

TEST_F (BuildingJsonTests, Inventories)
{
  ASSERT_EQ (tbl.CreateNew ("checkmark", "", Faction::ANCIENT)->GetId (), 1);

  inv.Get (1, "domob")->GetInventory ().SetFungibleCount ("foo", 2);
  inv.Get (42, "domob")->GetInventory ().SetFungibleCount ("foo", 100);
  inv.Get (1, "andy")->GetInventory ().SetFungibleCount ("bar", 1);

  DexOrderTable orders(db);
  orders.CreateNew (1, "domob", DexOrder::Type::ASK, "foo", 2, 10);
  orders.CreateNew (1, "domob", DexOrder::Type::ASK, "foo", 2, 15);
  orders.CreateNew (1, "domob", DexOrder::Type::ASK, "bar", 1, 10);
  orders.CreateNew (1, "andy", DexOrder::Type::ASK, "foo", 5, 20);
  orders.CreateNew (42, "domob", DexOrder::Type::ASK, "foo", 1, 1);
  orders.CreateNew (1, "domob", DexOrder::Type::BID, "foo", 1, 1);

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "inventories":
            {
              "andy": {"fungible": {"bar": 1}},
              "domob": {"fungible": {"foo": 2}}
            },
          "reserved":
            {
              "andy": {"fungible": {"foo": 5}},
              "domob": {"fungible": {"foo": 4, "bar": 1}}
            }
        }
      ]
  })");

  inv.Get (1, "domob")->GetInventory ().SetFungibleCount ("foo", 0);
  inv.Get (1, "andy")->GetInventory ().SetFungibleCount ("bar", 0);
  orders.DeleteForBuilding (1);

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "inventories":
            {
              "andy": null,
              "domob": null
            },
          "reserved":
            {
              "andy": null,
              "domob": null
            }
        }
      ]
  })");
}

TEST_F (BuildingJsonTests, Orderbook)
{
  ASSERT_EQ (tbl.CreateNew ("checkmark", "", Faction::ANCIENT)->GetId (), 1);

  db.SetNextId (101);
  DexOrderTable orders(db);
  orders.CreateNew (1, "domob", DexOrder::Type::BID, "foo", 2, 2);
  orders.CreateNew (1, "andy", DexOrder::Type::BID, "foo", 1, 2);
  orders.CreateNew (1, "domob", DexOrder::Type::BID, "foo", 1, 3);
  orders.CreateNew (1, "domob", DexOrder::Type::ASK, "foo", 1, 8);
  orders.CreateNew (1, "domob", DexOrder::Type::BID, "foo", 3, 1);
  orders.CreateNew (1, "domob", DexOrder::Type::ASK, "foo", 1, 10);
  orders.CreateNew (1, "domob", DexOrder::Type::BID, "bar", 1, 1);
  orders.CreateNew (42, "domob", DexOrder::Type::BID, "foo", 1, 5);
  orders.CreateNew (1, "domob", DexOrder::Type::ASK, "foo", 1, 9);

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "orderbook":
            {
              "bar":
                {
                  "item": "bar",
                  "bids":
                    [
                      {
                        "id": 107,
                        "account": "domob",
                        "quantity": 1,
                        "price": 1
                      }
                    ],
                  "asks": []
                },
              "foo":
                {
                  "item": "foo",
                  "bids":
                    [
                      {
                        "id": 103,
                        "account": "domob",
                        "quantity": 1,
                        "price": 3
                      },
                      {
                        "id": 102,
                        "account": "andy",
                        "quantity": 1,
                        "price": 2
                      },
                      {
                        "id": 101,
                        "account": "domob",
                        "quantity": 2,
                        "price": 2
                      },
                      {
                        "id": 105,
                        "account": "domob",
                        "quantity": 3,
                        "price": 1
                      }
                    ],
                  "asks":
                    [
                      {
                        "id": 104,
                        "account": "domob",
                        "quantity": 1,
                        "price": 8
                      },
                      {
                        "id": 109,
                        "account": "domob",
                        "quantity": 1,
                        "price": 9
                      },
                      {
                        "id": 106,
                        "account": "domob",
                        "quantity": 1,
                        "price": 10
                      }
                    ]
                }
            }
        }
      ]
  })");
}

TEST_F (BuildingJsonTests, ConfiguredFees)
{
  auto b = tbl.CreateNew ("checkmark", "daniel", Faction::RED);
  ASSERT_EQ (b->GetId (), 1);
  b.reset ();

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "config":
            {
              "servicefee": null,
              "dexfee": null
            }
        }
      ]
  })");

  b = tbl.GetById (1);
  b->MutableProto ().mutable_config ()->set_service_fee_percent (42);
  b->MutableProto ().mutable_config ()->set_dex_fee_bps (1'725);
  b.reset ();

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "config":
            {
              "servicefee": 42,
              "dexfee": 17.25
            }
        }
      ]
  })");
}

TEST_F (BuildingJsonTests, AgeData)
{
  auto b = tbl.CreateNew ("checkmark", "daniel", Faction::RED);
  ASSERT_EQ (b->GetId (), 1);
  b->MutableProto ().set_foundation (true);
  b->MutableProto ().mutable_age_data ()->set_founded_height (10);
  b.reset ();

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "age":
            {
              "founded": 10,
              "finished": null
            }
        }
      ]
  })");

  b = tbl.GetById (1);
  b->MutableProto ().set_foundation (false);
  b->MutableProto ().mutable_age_data ()->set_finished_height (12);
  b.reset ();

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "age":
            {
              "founded": 10,
              "finished": 12
            }
        }
      ]
  })");
}

TEST_F (BuildingJsonTests, CombatData)
{
  ASSERT_EQ (tbl.CreateNew ("checkmark", "", Faction::ANCIENT)->GetId (), 1);

  auto h = tbl.CreateNew ("checkmark", "daniel", Faction::RED);
  ASSERT_EQ (h->GetId (), 2);
  auto* att = h->MutableProto ().mutable_combat_data ()->add_attacks ();
  att->set_range (5);
  att->mutable_damage ()->set_min (1);
  att->mutable_damage ()->set_max (2);
  h->MutableHP ().set_armour (42);
  h->MutableHP ().mutable_mhp ()->set_shield (1);
  auto& regen = h->MutableRegenData ();
  regen.mutable_regeneration_mhp ()->set_shield (1'001);
  regen.mutable_max_hp ()->set_armour (100);
  regen.mutable_max_hp ()->set_shield (50);
  proto::TargetId t;
  t.set_id (10);
  t.set_type (proto::TargetId::TYPE_CHARACTER);
  h->SetTarget (t);
  h.reset ();

  ExpectStateJson (R"({
    "buildings":
      [
        {
          "id": 1,
          "combat": {"target": null}
        },
        {
          "id": 2,
          "combat":
            {
              "hp":
                {
                  "max": {"armour": 100, "shield": 50},
                  "current": {"armour": 42, "shield": 0.001},
                  "regeneration": {"shield": 1.001, "armour": 0}
                },
              "attacks":
                [
                  {"range": 5, "damage": {"min": 1, "max": 2}}
                ],
              "target": { "id": 10 }
            }
        }
      ]
  })");
}

/* ************************************************************************** */

class GroundLootJsonTests : public GameStateJsonTests
{

protected:

  GroundLootTable tbl;

  GroundLootJsonTests ()
    : tbl(db)
  {}

};

TEST_F (GroundLootJsonTests, Empty)
{
  ExpectStateJson (R"({
    "groundloot": []
  })");
}

TEST_F (GroundLootJsonTests, FungibleInventory)
{
  auto h = tbl.GetByCoord (HexCoord (1, 2));
  h->GetInventory ().SetFungibleCount ("foo", 5);
  h->GetInventory ().SetFungibleCount ("bar", 42);
  h->GetInventory ().SetFungibleCount ("", 100);
  h.reset ();

  h = tbl.GetByCoord (HexCoord (-1, 20));
  h->GetInventory ().SetFungibleCount ("foo", 10);
  h.reset ();

  ExpectStateJson (R"({
    "groundloot":
      [
        {
          "position": {"x": -1, "y": 20},
          "inventory":
            {
              "fungible":
                {
                  "foo": 10
                }
            }
        },
        {
          "position": {"x": 1, "y": 2},
          "inventory":
            {
              "fungible":
                {
                  "foo": 5,
                  "bar": 42,
                  "": 100
                }
            }
        }
      ]
  })");
}

/* ************************************************************************** */

class OngoingsJsonTests : public GameStateJsonTests
{

protected:

  OngoingsTable tbl;

  OngoingsJsonTests ()
    : tbl(db)
  {}

};

TEST_F (OngoingsJsonTests, Empty)
{
  ExpectStateJson (R"({
    "ongoings": []
  })");
}

TEST_F (OngoingsJsonTests, BasicData)
{
  auto op = tbl.CreateNew (3);
  ASSERT_EQ (op->GetId (), 1);
  op->SetHeight (5);
  op->SetCharacterId (42);
  op->MutableProto ().mutable_prospection ();
  op.reset ();

  op = tbl.CreateNew (5);
  op->SetHeight (10);
  op->SetBuildingId (50);
  op->MutableProto ().mutable_prospection ();
  op.reset ();

  ExpectStateJson (R"({
    "ongoings":
      [
        {
          "id": 1,
          "start_height": 3,
          "end_height": 5,
          "characterid": 42,
          "buildingid": null
        },
        {
          "id": 2,
          "start_height": 5,
          "end_height": 10,
          "characterid": null,
          "buildingid": 50
        }
      ]
  })");
}

TEST_F (OngoingsJsonTests, Prospection)
{
  auto op = tbl.CreateNew (1);
  ASSERT_EQ (op->GetId (), 1);
  op->MutableProto ().mutable_prospection ();
  op.reset ();

  ExpectStateJson (R"({
    "ongoings":
      [
        {
          "id": 1,
          "operation": "prospecting"
        }
      ]
  })");
}

TEST_F (OngoingsJsonTests, ArmourRepair)
{
  auto op = tbl.CreateNew (1);
  ASSERT_EQ (op->GetId (), 1);
  op->MutableProto ().mutable_armour_repair ();
  op.reset ();

  ExpectStateJson (R"({
    "ongoings":
      [
        {
          "id": 1,
          "operation": "armourrepair"
        }
      ]
  })");
}

TEST_F (OngoingsJsonTests, BlueprintCopy)
{
  ASSERT_EQ (GetBpCopyBlocks ("bow bpc", ctx), 1'000);

  auto op = tbl.CreateNew (1);
  ASSERT_EQ (op->GetId (), 1);
  op->SetHeight (1'001);
  auto& cp = *op->MutableProto ().mutable_blueprint_copy ();
  cp.set_account ("domob");
  cp.set_original_type ("bow bpo");
  cp.set_copy_type ("bow bpc");
  cp.set_num_copies (42);
  op.reset ();

  ExpectStateJson (R"({
    "ongoings":
      [
        {
          "id": 1,
          "operation": "bpcopy",
          "original": "bow bpo",
          "output": {"bow bpc": 42},
          "start_height": 1,
          "end_height": 42001
        }
      ]
  })");
}

TEST_F (OngoingsJsonTests, ItemConstruction)
{
  ASSERT_EQ (GetConstructionBlocks ("bow", ctx), 1'000);

  auto op = tbl.CreateNew (1);
  ASSERT_EQ (op->GetId (), 1);
  op->SetHeight (1'001);
  auto* c = op->MutableProto ().mutable_item_construction ();
  c->set_account ("domob");
  c->set_output_type ("bow");
  c->set_num_items (42);
  op.reset ();

  op = tbl.CreateNew (1);
  ASSERT_EQ (op->GetId (), 2);
  op->SetHeight (1'001);
  c = op->MutableProto ().mutable_item_construction ();
  c->set_account ("domob");
  c->set_output_type ("bow");
  c->set_num_items (5);
  c->set_original_type ("bow bpo");
  op.reset ();

  ExpectStateJson (R"({
    "ongoings":
      [
        {
          "id": 1,
          "operation": "construct",
          "output": {"bow": 42},
          "start_height": 1,
          "end_height": 1001
        },
        {
          "id": 2,
          "operation": "construct",
          "output": {"bow": 5},
          "original": "bow bpo",
          "start_height": 1,
          "end_height": 5001
        }
      ]
  })");
}

TEST_F (OngoingsJsonTests, BuildingConstruction)
{
  auto op = tbl.CreateNew (1);
  ASSERT_EQ (op->GetId (), 1);
  op->SetBuildingId (42);
  op->MutableProto ().mutable_building_construction ();
  op.reset ();

  ExpectStateJson (R"({
    "ongoings":
      [
        {
          "id": 1,
          "operation": "build",
          "buildingid": 42
        }
      ]
  })");
}

TEST_F (OngoingsJsonTests, BuildingConfigUpdate)
{
  auto op = tbl.CreateNew (1);
  ASSERT_EQ (op->GetId (), 1);
  op->SetBuildingId (42);
  op->MutableProto ().mutable_building_update ();
  op.reset ();

  ExpectStateJson (R"({
    "ongoings":
      [
        {
          "id": 1,
          "operation": "config",
          "newconfig":
            {
              "servicefee": null,
              "dexfee": null
            }
        }
      ]
  })");

  op = tbl.GetById (1);
  auto* upd = op->MutableProto ().mutable_building_update ();
  upd->mutable_new_config ()->set_service_fee_percent (2);
  upd->mutable_new_config ()->set_dex_fee_bps (150);
  op.reset ();

  ExpectStateJson (R"({
    "ongoings":
      [
        {
          "id": 1,
          "operation": "config",
          "newconfig":
            {
              "servicefee": 2,
              "dexfee": 1.5
            }
        }
      ]
  })");
}

/* ************************************************************************** */

class RegionJsonTests : public GameStateJsonTests
{

protected:

  RegionsTable tbl;

  RegionJsonTests ()
    : tbl(db, 1'042)
  {}

};

TEST_F (RegionJsonTests, Empty)
{
  /* This region is never changed to be non-trivial, and thus not in the result
     of the database query at all.  */
  tbl.GetById (10);

  /* This region ends up in a trivial state, but is written to the database
     because it is changed temporarily.  */
  tbl.GetById (20)->MutableProto ().set_prospecting_character (42);
  tbl.GetById (20)->MutableProto ().clear_prospecting_character ();


  ExpectStateJson (R"({
    "regions":
      [
        {
          "id": 20,
          "prospection": null,
          "resource": null
        }
      ]
  })");
}

TEST_F (RegionJsonTests, Prospection)
{
  tbl.GetById (20)->MutableProto ().set_prospecting_character (42);

  auto r = tbl.GetById (10);
  auto* prosp = r->MutableProto ().mutable_prospection ();
  prosp->set_name ("bar");
  prosp->set_height (107);
  r.reset ();

  ExpectStateJson (R"({
    "regions":
      [
        {
          "id": 10,
          "prospection":
            {
              "name": "bar",
              "height": 107
            }
        },
        {"id": 20, "prospection": {"inprogress": 42}}
      ]
  })");
}

TEST_F (RegionJsonTests, MiningResource)
{
  auto r = tbl.GetById (10);
  r->MutableProto ().mutable_prospection ()->set_resource ("sand");
  r->SetResourceLeft (150);
  r.reset ();

  ExpectStateJson (R"({
    "regions":
      [
        {
          "id": 10,
          "resource":
            {
              "type": "sand",
              "amount": 150
            }
        }
      ]
  })");
}

/* ************************************************************************** */

class MoneySupplyJsonTests : public GameStateJsonTests
{

protected:

  MoneySupply ms;

  MoneySupplyJsonTests ()
    : ms(db)
  {}

};

TEST_F (MoneySupplyJsonTests, EntriesAndTotal)
{
  ctx.SetChain (xaya::Chain::REGTEST);

  ms.Increment ("burnsale", 20);
  ms.Increment ("gifted", 10);

  ExpectStateJson (R"({
    "moneysupply":
      {
        "total": 30,
        "entries":
          {
            "gifted": 10,
            "burnsale": 20
          }
      }
  })");
}

TEST_F (MoneySupplyJsonTests, GiftedOnMainnet)
{
  ctx.SetChain (xaya::Chain::MAIN);

  ms.Increment ("burnsale", 20);

  ExpectStateJson (R"({
    "moneysupply":
      {
        "total": 20,
        "entries":
          {
            "gifted": null,
            "burnsale": 20
          }
      }
  })");
}

TEST_F (MoneySupplyJsonTests, BurnsaleStages)
{
  ms.Increment ("burnsale", 25'000'000'000);

  ExpectStateJson (R"({
    "moneysupply":
      {
        "total": 25000000000,
        "entries":
          {
            "burnsale": 25000000000
          },
        "burnsale":
          [
            {
              "stage": 1,
              "price": 0.0001,
              "total": 10000000000,
              "sold": 10000000000,
              "available": 0
            },
            {
              "stage": 2,
              "price": 0.0002,
              "total": 10000000000,
              "sold": 10000000000,
              "available": 0
            },
            {
              "stage": 3,
              "price": 0.0005,
              "total": 10000000000,
              "sold": 5000000000,
              "available": 5000000000
            },
            {
              "stage": 4,
              "price": 0.0010,
              "total": 20000000000,
              "sold": 0,
              "available": 20000000000
            }
          ]
      }
  })");
}

/* ************************************************************************** */

class PrizesJsonTests : public GameStateJsonTests
{

protected:

  ItemCounts cnt;

  PrizesJsonTests ()
    : cnt(db)
  {}

};

TEST_F (PrizesJsonTests, Works)
{
  cnt.IncrementFound ("gold prize");
  for (unsigned i = 0; i < 10; ++i)
    cnt.IncrementFound ("silver prize");

  ExpectStateJson (R"({
    "prizes":
      {
        "gold":
          {
            "number": 3,
            "probability": 100,
            "found": 1,
            "available": 2
          },
        "silver":
          {
            "number": 1000,
            "probability": 10,
            "found": 10,
            "available": 990
          },
        "bronze":
          {
            "number": 1,
            "probability": 1,
            "found": 0,
            "available": 1
          }
      }
  })");
}

/* ************************************************************************** */

class TradeHistoryJsonTests : public GameStateJsonTests
{

protected:

  DexHistoryTable history;

  TradeHistoryJsonTests ()
    : history(db)
  {
    history.RecordTrade (10, 1'024, 42, "foo", 2, 3, "domob", "andy");
    history.RecordTrade (9, 987, 42, "foo", 5, 3, "andy", "domob");
    history.RecordTrade (10, 1'024, 10, "foo", 1, 1, "domob", "andy");
    history.RecordTrade (10, 1'024, 42, "bar", 1, 1, "domob", "andy");
  }

};

TEST_F (TradeHistoryJsonTests, NoEntry)
{
  EXPECT_TRUE (PartialJsonEqual (converter.TradeHistory ("foo", 100),
                                 ParseJson ("[]")));
  EXPECT_TRUE (PartialJsonEqual (converter.TradeHistory ("zerospace", 42),
                                 ParseJson ("[]")));
}

TEST_F (TradeHistoryJsonTests, ForItemAndBuilding)
{
  EXPECT_TRUE (PartialJsonEqual (converter.TradeHistory ("foo", 42),
                                 ParseJson (R"([
    {
      "height": 10,
      "timestamp": 1024,
      "buildingid": 42,
      "item": "foo",
      "quantity": 2,
      "price": 3,
      "cost": 6,
      "seller": "domob",
      "buyer": "andy"
    },
    {
      "height": 9,
      "timestamp": 987,
      "buildingid": 42,
      "item": "foo",
      "quantity": 5,
      "price": 3,
      "cost": 15,
      "seller": "andy",
      "buyer": "domob"
    }
  ])")));
}

/* ************************************************************************** */

} // anonymous namespace
} // namespace pxd
