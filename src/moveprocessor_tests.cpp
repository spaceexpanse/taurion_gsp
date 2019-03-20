#include "moveprocessor.hpp"

#include "jsonutils.hpp"
#include "params.hpp"
#include "protoutils.hpp"

#include "database/dbtest.hpp"

#include <gtest/gtest.h>

#include <json/json.h>

#include <sstream>
#include <string>

namespace pxd
{
namespace
{

#define DEVADDR "DHy2615XKevE23LVRVZVxGeqxadRGyiFW4"

class MoveProcessorTests : public DBTestWithSchema
{

protected:

  /** Params instance that is used.  Set to mainnet.  */
  const Params params;

  /** Basemap instance for use in tests.  */
  const BaseMap map;

private:

  /** MoveProcessor instance for use in the test.  */
  MoveProcessor mvProc;

protected:

  MoveProcessorTests ()
    : params(xaya::Chain::MAIN), mvProc(db, params, map)
  {}

  /**
   * Processes the given data (which is passed as string and converted to
   * JSON before processing it).
   */
  void
  Process (const std::string& str)
  {
    Json::Value val;
    std::istringstream in(str);
    in >> val;

    mvProc.ProcessAll (val);
  }

  /**
   * Processes the given data as string, adding the given amount as payment
   * to the dev address for each entry.  This is a utility method to avoid the
   * need of pasting in the long "out" and dev-address parts.
   */
  void
  ProcessWithDevPayment (const std::string& str, const Amount amount)
  {
    Json::Value val;
    std::istringstream in(str);
    in >> val;

    for (auto& entry : val)
      entry["out"][DEVADDR] = AmountToJson (amount);

    mvProc.ProcessAll (val);
  }

};

/* ************************************************************************** */

TEST_F (MoveProcessorTests, InvalidDataFromXaya)
{
  EXPECT_DEATH (Process ("{}"), "isArray");

  EXPECT_DEATH (Process (R"(
    [{"name": "domob"}]
  )"), "isMember.*move");

  EXPECT_DEATH (Process (R"(
    [{"move": {}}]
  )"), "nameVal.isString");
  EXPECT_DEATH (Process (R"(
    [{"name": 5, "move": {}}]
  )"), "nameVal.isString");

  EXPECT_DEATH (Process (R"([{
    "name": "domob", "move": {},
    "out": {")" DEVADDR R"(": false}
  }])"), "JSON value for amount is not double");
}

/* ************************************************************************** */

class CharacterCreationTests : public MoveProcessorTests
{

protected:

  /** Character table used in the tests.  */
  CharacterTable tbl;

  CharacterCreationTests ()
    : tbl(db)
  {}

};

TEST_F (CharacterCreationTests, InvalidCommands)
{
  ProcessWithDevPayment (R"([
    {"name": "domob", "move": {}},
    {"name": "domob", "move": {"nc": 42}},
    {"name": "domob", "move": {"nc": {}}},
    {"name": "domob", "move":
      {
        "nc": {"faction": "r", "other": false}
      }},
    {"name": "domob", "move": {"nc": {"faction": "x"}}},
    {"name": "domob", "move": {"nc": {"faction": 0}}}
  ])", params.CharacterCost ());

  auto res = tbl.QueryAll ();
  EXPECT_FALSE (res.Step ());
}

TEST_F (CharacterCreationTests, ValidCreation)
{
  ProcessWithDevPayment (R"([
    {"name": "domob", "move": {"nc": {"faction": "r"}}},
    {"name": "domob", "move": {"nc": {"faction": "g"}}},
    {"name": "andy", "move": {"nc": {"faction": "b"}}}
  ])", params.CharacterCost ());

  auto res = tbl.QueryAll ();

  ASSERT_TRUE (res.Step ());
  auto c = tbl.GetFromResult (res);
  EXPECT_EQ (c->GetOwner (), "domob");
  EXPECT_EQ (c->GetFaction (), Faction::RED);

  ASSERT_TRUE (res.Step ());
  c = tbl.GetFromResult (res);
  EXPECT_EQ (c->GetOwner (), "domob");
  EXPECT_EQ (c->GetFaction (), Faction::GREEN);

  ASSERT_TRUE (res.Step ());
  c = tbl.GetFromResult (res);
  EXPECT_EQ (c->GetOwner (), "andy");
  EXPECT_EQ (c->GetFaction (), Faction::BLUE);

  EXPECT_FALSE (res.Step ());
}

TEST_F (CharacterCreationTests, DevPayment)
{
  Process (R"([
    {"name": "domob", "move": {"nc": {"faction": "r"}}}
  ])");
  ProcessWithDevPayment (R"([
    {"name": "domob", "move": {"nc": {"faction": "g"}}}
  ])", params.CharacterCost () - 1);
  ProcessWithDevPayment (R"([
    {"name": "domob", "move": {"nc": {"faction": "b"}}}
  ])", params.CharacterCost () + 1);

  auto res = tbl.QueryAll ();
  ASSERT_TRUE (res.Step ());
  auto c = tbl.GetFromResult (res);
  EXPECT_EQ (c->GetOwner (), "domob");
  EXPECT_EQ (c->GetFaction (), Faction::BLUE);
  EXPECT_FALSE (res.Step ());
}

/* ************************************************************************** */

class CharacterUpdateTests : public MoveProcessorTests
{

protected:

  /** Character table to be used in tests.  */
  CharacterTable tbl;

  /**
   * Retrieves a handle to the test character.
   */
  CharacterTable::Handle
  GetTest ()
  {
    auto h = tbl.GetById (1);
    CHECK (h != nullptr);
    return h;
  }

  /**
   * Inserts a new character with the given ID, name and owner.
   */
  void
  SetupCharacter (const Database::IdT id, const std::string& owner)
  {
    db.SetNextId (id);
    tbl.CreateNew (owner, Faction::RED);

    auto h = tbl.GetById (id);
    CHECK (h != nullptr);
    CHECK_EQ (h->GetId (), id);
    CHECK_EQ (h->GetOwner (), owner);
  }

  /**
   * All CharacterUpdateTests will start with a test character already created.
   * We also ensure that it has the ID 1.
   */
  CharacterUpdateTests ()
    : tbl(db)
  {
    SetupCharacter (1, "domob");
  }

};

TEST_F (CharacterUpdateTests, CreationAndUpdate)
{
  ProcessWithDevPayment (R"([{
    "name": "domob",
    "move":
      {
        "nc": {"faction": "r"},
        "c": {"1": {"send": "daniel"}, "2": {"send": "andy"}}
      }
  }])", params.CharacterCost ());

  /* Transfer and creation should work fine together for two different
     characters (but in the same move).  */
  EXPECT_EQ (GetTest ()->GetOwner (), "daniel");

  /* The character created in the same move should not be transferred.  */
  auto h = tbl.GetById (2);
  ASSERT_TRUE (h != nullptr);
  EXPECT_EQ (h->GetOwner (), "domob");
}

TEST_F (CharacterUpdateTests, ValidTransfer)
{
  EXPECT_EQ (GetTest ()->GetOwner (), "domob");
  Process (R"([{
    "name": "domob",
    "move": {"c": {"1": {"send": "andy"}}}
  }])");
  EXPECT_EQ (GetTest ()->GetOwner (), "andy");
}

TEST_F (CharacterUpdateTests, InvalidTransfer)
{
  Process (R"([{
    "name": "domob",
    "move": {"c": {"1": {"send": false}}}
  }])");
  EXPECT_EQ (GetTest ()->GetOwner (), "domob");
}

TEST_F (CharacterUpdateTests, OwnerCheck)
{
  /* Verify that a later update works fine even if a previous character update
     (from the same move) failed due to the owner check.  */
  SetupCharacter (9, "andy");

  EXPECT_EQ (GetTest ()->GetOwner (), "domob");
  EXPECT_EQ (tbl.GetById (9)->GetOwner (), "andy");
  Process (R"([{
    "name": "andy",
    "move": {"c": {"1": {"send": "andy"}, "9": {"send": "domob"}}}
  }])");
  EXPECT_EQ (GetTest ()->GetOwner (), "domob");
  EXPECT_EQ (tbl.GetById (9)->GetOwner (), "domob");
}

TEST_F (CharacterUpdateTests, InvalidUpdate)
{
  /* We want to test that one invalid update still allows for other
     updates (i.e. other characters) to be done successfully in the same
     move transaction.  Thus create another character with a later ID that
     we will use for successful updates.  */
  SetupCharacter (9, "domob");

  for (const std::string upd : {"\"1\": []", "\"1\": false",
                                R"(" ": {"send": "andy"})",
                                R"("5": {"send": "andy"})"})
    {
      ASSERT_EQ (tbl.GetById (9)->GetOwner (), "domob");
      Process (R"([{
        "name": "domob",
        "move": {"c":{
          )" + upd + R"(,
          "9": {"send": "andy"}
        }}
      }])");

      auto h = tbl.GetById (9);
      EXPECT_EQ (h->GetOwner (), "andy");
      h->SetOwner ("domob");
    }
}

TEST_F (CharacterUpdateTests, WhenBusy)
{
  auto h = GetTest ();
  h->SetBusy (2);
  h->MutableProto ().mutable_prospection ();
  h.reset ();

  Process (R"([
    {
      "name": "domob",
      "move": {"c": {"1": {"wp": [{"x": -3, "y": 4}]}}}
    },
    {
      "name": "domob",
      "move": {"c": {"1": {"prospect": {}}}}
    }
  ])");

  h = GetTest ();
  /* The fresh prospect command should have been ignored.  If it were not,
     then busy would have been set to 10.  */
  EXPECT_EQ (h->GetBusy (), 2);
  EXPECT_FALSE (h->GetProto ().has_movement ());
}

TEST_F (CharacterUpdateTests, Waypoints)
{
  /* Set up some stuff that will be cleared.  */
  auto h = GetTest ();
  h->SetPartialStep (42);
  auto* mv = h->MutableProto ().mutable_movement ();
  mv->mutable_waypoints ()->Add ();
  mv->mutable_steps ()->Add ();
  h.reset ();

  /* Run some moves that are invalid one way or another.  */
  Process (R"([
    {
      "name": "domob",
      "move": {"c": {"1": {"wp": "foo"}}}
    },
    {
      "name": "domob",
      "move": {"c": {"1": {"wp": {"x": 4, "y": 3}}}}
    },
    {
      "name": "domob",
      "move": {"c": {"1": {"wp": [{"x": 4.5, "y": 3}]}}}
    },
    {
      "name": "andy",
      "move": {"c": {"1": {"wp": [{"x": 4, "y": 3}]}}}
    }
  ])");

  /* Verify that we still have the original stuff (i.e. the invalid moves
     had no effect at all).  */
  h = GetTest ();
  EXPECT_EQ (h->GetPartialStep (), 42);
  EXPECT_EQ (h->GetProto ().movement ().waypoints_size (), 1);
  EXPECT_EQ (h->GetProto ().movement ().steps_size (), 1);
  h.reset ();

  /* Process a valid waypoints update move.  */
  Process (R"([{
    "name": "domob",
    "move": {"c": {"1": {"wp": [{"x": -3, "y": 4}, {"x": 5, "y": 0}]}}}
  }])");

  /* Verify that the valid move had the expected effect.  */
  h = GetTest ();
  EXPECT_EQ (h->GetPartialStep (), 0);
  EXPECT_EQ (h->GetProto ().movement ().steps_size (), 0);
  const auto& wp = h->GetProto ().movement ().waypoints ();
  ASSERT_EQ (wp.size (), 2);
  EXPECT_EQ (CoordFromProto (wp.Get (0)), HexCoord (-3, 4));
  EXPECT_EQ (CoordFromProto (wp.Get (1)), HexCoord (5, 0));
}

class ProspectingMoveTests : public CharacterUpdateTests
{

protected:

  /** Regions table instance for testing.  */
  RegionsTable regions;

  /** Position for the test character.  */
  const HexCoord pos;

  /** Region of the test position.  */
  const RegionMap::IdT region;

  ProspectingMoveTests ()
    : regions(db), pos(-10, 42), region(map.Regions ().GetRegionId (pos))
  {
    GetTest ()->SetPosition (pos);
  }

};

TEST_F (ProspectingMoveTests, Success)
{
  auto h = GetTest ();
  h->SetPartialStep (42);
  h->MutableProto ().mutable_movement ()->add_waypoints ();

  Process (R"([
    {
      "name": "domob",
      "move": {"c": {"1": {
        "wp": [{"x": 5, "y": -2}],
        "prospect": {}
      }}}
    }
  ])");

  h = GetTest ();
  EXPECT_EQ (h->GetBusy (), 10);
  EXPECT_TRUE (h->GetProto ().has_prospection ());
  EXPECT_EQ (h->GetPartialStep (), 0);
  EXPECT_FALSE (h->GetProto ().has_movement ());

  auto r = regions.GetById (region);
  EXPECT_EQ (r->GetProto ().prospecting_character (), 1);
  EXPECT_FALSE (r->GetProto ().has_prospection ());
}

TEST_F (ProspectingMoveTests, Invalid)
{
  GetTest ()->MutableProto ().mutable_movement ()->add_waypoints ();

  Process (R"([
    {
      "name": "domob",
      "move": {"c": {"1": {"prospect": true}}}
    },
    {
      "name": "domob",
      "move": {"c": {"1": {"prospect": 1}}}
    },
    {
      "name": "domob",
      "move": {"c": {"1": {"prospect": {"x": 42}}}}
    }
  ])");

  auto h = GetTest ();
  EXPECT_EQ (h->GetBusy (), 0);
  EXPECT_FALSE (h->GetProto ().has_prospection ());
  EXPECT_TRUE (h->GetProto ().has_movement ());

  auto r = regions.GetById (region);
  EXPECT_FALSE (r->GetProto ().has_prospecting_character ());
  EXPECT_FALSE (r->GetProto ().has_prospection ());
}

TEST_F (ProspectingMoveTests, RegionAlreadyProspected)
{
  auto r = regions.GetById (region);
  r->MutableProto ().mutable_prospection ()->set_name ("foo");
  r.reset ();

  Process (R"([
    {
      "name": "domob",
      "move": {"c": {"1": {"prospect": {}}}}
    }
  ])");

  auto h = GetTest ();
  EXPECT_EQ (h->GetBusy (), 0);
  EXPECT_FALSE (h->GetProto ().has_prospection ());

  r = regions.GetById (region);
  EXPECT_FALSE (r->GetProto ().has_prospecting_character ());
  EXPECT_EQ (r->GetProto ().prospection ().name (), "foo");
}

TEST_F (ProspectingMoveTests, MultipleCharacters)
{
  auto c = tbl.CreateNew ("foo", Faction::RED);
  ASSERT_EQ (c->GetId (), 2);
  c->SetPosition (pos);
  c.reset ();

  Process (R"([
    {
      "name": "foo",
      "move": {"c": {"2": {"prospect": {}}}}
    },
    {
      "name": "domob",
      "move": {"c": {"1": {"prospect": {}}}}
    }
  ])");

  c = tbl.GetById (1);
  ASSERT_EQ (c->GetOwner (), "domob");
  EXPECT_EQ (c->GetBusy (), 0);

  c = tbl.GetById (2);
  ASSERT_EQ (c->GetOwner (), "foo");
  EXPECT_EQ (c->GetBusy (), 10);

  auto r = regions.GetById (region);
  EXPECT_EQ (r->GetProto ().prospecting_character (), 2);
}

/* ************************************************************************** */

} // anonymous namespace
} // namespace pxd
