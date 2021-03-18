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

#include "buildings.hpp"
#include "jsonutils.hpp"
#include "modifier.hpp"
#include "protoutils.hpp"
#include "services.hpp"

#include "database/account.hpp"
#include "database/building.hpp"
#include "database/character.hpp"
#include "database/faction.hpp"
#include "database/itemcounts.hpp"
#include "database/moneysupply.hpp"
#include "database/ongoing.hpp"
#include "database/region.hpp"
#include "hexagonal/pathfinder.hpp"
#include "proto/character.pb.h"

#include <algorithm>

namespace pxd
{

namespace
{

/**
 * Converts a TargetId proto to its JSON gamestate form.  It may be a null
 * JSON if the target is "empty".
 */
Json::Value
TargetIdToJson (const proto::TargetId& target)
{
  Json::Value res(Json::objectValue);
  res["id"] = IntToJson (target.id ());

  switch (target.type ())
    {
    case proto::TargetId::TYPE_CHARACTER:
      res["type"] = "character";
      break;
    case proto::TargetId::TYPE_BUILDING:
      res["type"] = "building";
      break;
    default:
      LOG (FATAL) << "Invalid target type: " << target.type ();
    }

  return res;
}

/**
 * Converts an HP value (composed of full base HP and millis) to
 * the corresponding JSON representation.
 */
Json::Value
HpValueToJson (const unsigned full, const unsigned millis)
{
  if (millis == 0)
    return IntToJson (full);

  return full + millis / 1'000.0;
}

/**
 * Converts an HP proto to a JSON form.
 */
Json::Value
HpProtoToJson (const proto::HP& hp)
{
  Json::Value res(Json::objectValue);
  res["armour"] = HpValueToJson (hp.armour (), hp.mhp ().armour ());
  res["shield"] = HpValueToJson (hp.shield (), hp.mhp ().shield ());
  return res;
}

/**
 * Computes the "movement" sub-object for a Character's JSON state.
 */
Json::Value
GetMovementJsonObject (const Character& c)
{
  const auto& pb = c.GetProto ();
  Json::Value res(Json::objectValue);

  const auto& volMv = c.GetVolatileMv ();
  if (volMv.has_partial_step ())
    res["partialstep"] = IntToJson (volMv.partial_step ());
  if (volMv.has_blocked_turns ())
    res["blockedturns"] = IntToJson (volMv.blocked_turns ());

  if (pb.has_movement ())
    {
      const auto& mvProto = pb.movement ();

      if (mvProto.has_chosen_speed ())
        res["chosenspeed"] = mvProto.chosen_speed ();

      Json::Value wp(Json::arrayValue);
      for (const auto& entry : mvProto.waypoints ())
        wp.append (CoordToJson (CoordFromProto (entry)));
      if (wp.size () > 0)
        res["waypoints"] = wp;
    }

  return res;
}

/**
 * Computes the basic "combat" sub-object for a Character or Building.
 */
Json::Value
GetCombatJsonObject (const CombatEntity& h)
{
  Json::Value res(Json::objectValue);

  if (h.HasTarget ())
    res["target"] = TargetIdToJson (h.GetTarget ());

  const auto& pb = h.GetCombatData ();
  Json::Value attacks(Json::arrayValue);
  for (const auto& attack : pb.attacks ())
    {
      Json::Value obj(Json::objectValue);
      if (attack.has_range ())
        obj["range"] = IntToJson (attack.range ());
      if (attack.has_area ())
        obj["area"] = IntToJson (attack.area ());
      if (attack.friendlies ())
        obj["friendlies"] = true;

      if (attack.has_damage ())
        {
          Json::Value dmg(Json::objectValue);
          dmg["min"] = IntToJson (attack.damage ().min ());
          dmg["max"] = IntToJson (attack.damage ().max ());
          obj["damage"] = dmg;
        }

      attacks.append (obj);
    }
  if (!attacks.empty ())
    res["attacks"] = attacks;

  const auto& regen = h.GetRegenData ();
  Json::Value hp(Json::objectValue);
  hp["max"] = HpProtoToJson (regen.max_hp ());
  hp["current"] = HpProtoToJson (h.GetHP ());

  proto::HP fakeRegen;
  *fakeRegen.mutable_mhp () = regen.regeneration_mhp ();
  hp["regeneration"] = HpProtoToJson (fakeRegen);

  res["hp"] = hp;

  return res;
}

/**
 * Computes the "combat" sub-object for a Character's JSON state
 * (which includes the attackers data from DamageLists).
 */
Json::Value
GetCombatJsonObject (const Character& c, const DamageLists& dl)
{
  Json::Value res = GetCombatJsonObject (c);

  Json::Value attackers(Json::arrayValue);
  for (const auto id : dl.GetAttackers (c.GetId ()))
    attackers.append (IntToJson (id));
  if (!attackers.empty ())
    res["attackers"] = attackers;

  return res;
}

/**
 * Constructs the JSON representation of a character's cargo space.
 */
Json::Value
GetCargoSpaceJsonObject (const Character& c, const Context& ctx)
{
  const auto used = c.UsedCargoSpace (ctx.RoConfig ());

  Json::Value res(Json::objectValue);
  res["total"] = IntToJson (c.GetProto ().cargo_space ());
  res["used"] = IntToJson (used);
  res["free"] = IntToJson (c.GetProto ().cargo_space () - used);

  return res;
}

/**
 * Constructs the JSON representation of the mining data of a character.
 */
Json::Value
GetMiningJsonObject (const BaseMap& map, const Character& c)
{
  if (!c.GetProto ().has_mining ())
    return Json::Value ();
  const auto& pb = c.GetProto ().mining ();

  Json::Value rate(Json::objectValue);
  rate["min"] = IntToJson (pb.rate ().min ());
  rate["max"] = IntToJson (pb.rate ().max ());

  Json::Value res(Json::objectValue);
  res["rate"] = rate;
  res["active"] = pb.active ();
  if (pb.active ())
    res["region"] = IntToJson (map.Regions ().GetRegionId (c.GetPosition ()));

  return res;
}

} // anonymous namespace

template <>
  Json::Value
  GameStateJson::Convert<Inventory> (const Inventory& inv) const
{
  Json::Value fungible(Json::objectValue);
  for (const auto& entry : inv.GetFungible ())
    fungible[entry.first] = IntToJson (entry.second);

  Json::Value res(Json::objectValue);
  res["fungible"] = fungible;

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<Character> (const Character& c) const
{
  Json::Value res(Json::objectValue);
  res["id"] = IntToJson (c.GetId ());
  res["owner"] = c.GetOwner ();
  res["faction"] = FactionToString (c.GetFaction ());
  res["vehicle"] = c.GetProto ().vehicle ();

  Json::Value fitments(Json::arrayValue);
  for (const auto& f : c.GetProto ().fitments ())
    fitments.append (f);
  res["fitments"] = fitments;

  if (c.IsInBuilding ())
    res["inbuilding"] = IntToJson (c.GetBuildingId ());
  else
    res["position"] = CoordToJson (c.GetPosition ());

  if (c.GetEnterBuilding () != Database::EMPTY_ID)
    res["enterbuilding"] = IntToJson (c.GetEnterBuilding ());

  res["combat"] = GetCombatJsonObject (c, dl);
  res["speed"] = c.GetProto ().speed ();
  res["inventory"] = Convert (c.GetInventory ());
  res["cargospace"] = GetCargoSpaceJsonObject (c, ctx);

  const Json::Value mv = GetMovementJsonObject (c);
  if (!mv.empty ())
    res["movement"] = mv;

  if (c.IsBusy ())
    res["busy"] = IntToJson (c.GetProto ().ongoing ());

  const Json::Value mining = GetMiningJsonObject (ctx.Map (), c);
  if (!mining.isNull ())
    res["mining"] = mining;

  const auto& pb = c.GetProto ();
  if (pb.has_prospecting_blocks ())
    res["prospectingblocks"] = IntToJson (pb.prospecting_blocks ());

  if (pb.has_refining ())
    {
      Json::Value ref(Json::objectValue);
      const StatModifier inputMod(pb.refining ().input ());
      ref["inefficiency"] = IntToJson (inputMod (100));
      res["refining"] = ref;
    }

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<Account> (const Account& a) const
{
  const auto& pb = a.GetProto ();
  const auto& cfg = ctx.RoConfig ();

  Json::Value res(Json::objectValue);
  res["name"] = a.GetName ();
  res["minted"] = IntToJson (pb.burnsale_balance ());

  Json::Value bal(Json::objectValue);
  bal["available"] = IntToJson (a.GetBalance ());
  res["balance"] = bal;

  if (!a.IsInitialised ())
    return res;

  res["faction"] = FactionToString (a.GetFaction ());
  res["kills"] = IntToJson (pb.kills ());
  res["fame"] = IntToJson (pb.fame ());

  Json::Value skills(Json::objectValue);
  for (const auto s : cfg.AllSkillTypes ())
    {
      const auto& data = a.Skills ()[s];

      Json::Value cur(Json::objectValue);
      cur["xp"] = IntToJson (data.GetXp ());

      skills[cfg.Skill (s).name ()] = cur;
    }
  res["skills"] = skills;

  return res;
}

Json::Value
GameStateJson::Convert (const proto::Building::Config& cfg)
{
  Json::Value res(Json::objectValue);
  if (cfg.has_service_fee_percent ())
    res["servicefee"] = IntToJson (cfg.service_fee_percent ());
  if (cfg.has_dex_fee_bps ())
    res["dexfee"] = cfg.dex_fee_bps () / 100.0;
  return res;
}

namespace
{

/**
 * Builds up the orderbook of the DEX inside a given building and returns
 * it as JSON.
 */
Json::Value
GetOrderbookInBuilding (DexOrderTable& orders, const Database::IdT building)
{
  Json::Value book(Json::objectValue);

  auto res = orders.QueryForBuilding (building);
  while (res.Step ())
    {
      const auto o = orders.GetFromResult (res);

      if (!book.isMember (o->GetItem ()))
        {
          Json::Value freshItem(Json::objectValue);
          freshItem["item"] = o->GetItem ();
          freshItem["bids"] = Json::Value (Json::arrayValue);
          freshItem["asks"] = Json::Value (Json::arrayValue);
          book[o->GetItem ()] = freshItem;
        }
      auto& itm = book[o->GetItem ()];
      CHECK (itm.isObject ());

      Json::Value cur(Json::objectValue);
      cur["id"] = IntToJson (o->GetId ());
      cur["account"] = o->GetAccount ();
      cur["quantity"] = IntToJson (o->GetQuantity ());
      cur["price"] = IntToJson (o->GetPrice ());

      std::string key;
      switch (o->GetType ())
        {
        case DexOrder::Type::BID:
          key = "bids";
          break;
        case DexOrder::Type::ASK:
          key = "asks";
          break;
        default:
          LOG (FATAL)
              << "Invalid order type: " << static_cast<int> (o->GetType ());
        }

      auto& orders = itm[key];
      CHECK (orders.isArray ());
      orders.append (cur);
    }

  /* QueryForBuilding orders the results increasing by price.  This means that
     we want to reverse the order of all bids (so the best is listed first).  */
  for (auto it = book.begin (); it != book.end (); ++it)
    {
      auto& bids = (*it)["bids"];
      CHECK (bids.isArray ());
      std::reverse (bids.begin (), bids.end ());
    }

  return book;
}

} // anonymous namespace

template <>
  Json::Value
  GameStateJson::Convert<Building> (const Building& b) const
{
  const auto& pb = b.GetProto ();

  Json::Value res(Json::objectValue);
  res["id"] = IntToJson (b.GetId ());
  res["type"] = b.GetType ();
  if (pb.foundation ())
    res["foundation"] = true;

  res["faction"] = FactionToString (b.GetFaction ());
  if (b.GetFaction () != Faction::ANCIENT)
    res["owner"] = b.GetOwner ();
  res["centre"] = CoordToJson (b.GetCentre ());

  res["rotationsteps"] = IntToJson (pb.shape_trafo ().rotation_steps ());
  res["config"] = Convert (pb.config ());

  Json::Value tiles(Json::arrayValue);
  for (const auto& c : GetBuildingShape (b, ctx))
    tiles.append (CoordToJson (c));
  res["tiles"] = tiles;

  res["combat"] = GetCombatJsonObject (b);

  if (pb.foundation ())
    {
      Json::Value constr(Json::objectValue);
      if (pb.has_ongoing_construction ())
        constr["ongoing"] = IntToJson (pb.ongoing_construction ());
      constr["inventory"] = Convert (Inventory (pb.construction_inventory ()));
      res["construction"] = constr;
    }
  else
    {
      auto invRes = buildingInventories.QueryForBuilding (b.GetId ());
      Json::Value inv(Json::objectValue);
      while (invRes.Step ())
        {
          auto h = buildingInventories.GetFromResult (invRes);
          inv[h->GetAccount ()] = Convert (h->GetInventory ());
        }
      res["inventories"] = inv;

      Json::Value reserved(Json::objectValue);
      for (const auto& entry : orders.GetReservedQuantities (b.GetId ()))
        reserved[entry.first] = Convert (entry.second);
      res["reserved"] = reserved;

      res["orderbook"] = GetOrderbookInBuilding (orders, b.GetId ());
    }

  Json::Value age(Json::objectValue);
  age["founded"] = IntToJson (pb.age_data ().founded_height ());
  if (!pb.foundation ())
    age["finished"] = IntToJson (pb.age_data ().finished_height ());
  res["age"] = age;

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<pxd::GroundLoot> (const pxd::GroundLoot& loot) const
{
  Json::Value res(Json::objectValue);
  res["position"] = CoordToJson (loot.GetPosition ());
  res["inventory"] = Convert (loot.GetInventory ());

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<pxd::OngoingOperation> (
      const pxd::OngoingOperation& op) const
{
  Json::Value res(Json::objectValue);
  const auto& pb = op.GetProto ();

  res["id"] = IntToJson (op.GetId ());
  res["start_height"] = IntToJson (pb.start_height ());
  if (op.GetCharacterId () != Database::EMPTY_ID)
    res["characterid"] = IntToJson (op.GetCharacterId ());
  if (op.GetBuildingId () != Database::EMPTY_ID)
    res["buildingid"] = IntToJson (op.GetBuildingId ());

  /* For some operations, the actual end height is not the height of the
     operation, because it will be processed multiple times before it is
     complete (e.g. blueprint copies).  For them, we add an operation-specific
     delta onto the operation height.  */
  unsigned endDelta = 0;

  switch (pb.op_case ())
    {
    case proto::OngoingOperation::kProspection:
      res["operation"] = "prospecting";
      break;

    case proto::OngoingOperation::kArmourRepair:
      res["operation"] = "armourrepair";
      break;

    case proto::OngoingOperation::kBlueprintCopy:
      {
        const auto& cp = pb.blueprint_copy ();

        res["operation"] = "bpcopy";
        res["account"] = cp.account ();
        res["original"] = cp.original_type ();

        Json::Value output(Json::objectValue);
        output[cp.copy_type ()] = IntToJson (cp.num_copies ());
        res["output"] = output;

        CHECK_GE (cp.num_copies (), 1);
        endDelta += (cp.num_copies () - 1)
                      * GetBpCopyBlocks (cp.copy_type (), ctx);

        break;
      }

    case proto::OngoingOperation::kItemConstruction:
      {
        const auto& c = pb.item_construction ();

        res["operation"] = "construct";
        res["account"] = c.account ();

        Json::Value output(Json::objectValue);
        output[c.output_type ()] = IntToJson (c.num_items ());
        res["output"] = output;

        if (c.has_original_type ())
          {
            res["original"] = c.original_type ();

            CHECK_GE (c.num_items (), 1);
            endDelta += (c.num_items () - 1)
                          * GetConstructionBlocks (c.output_type (), ctx);
          }

        break;
      }

    case proto::OngoingOperation::kBuildingConstruction:
      res["operation"] = "build";
      break;

    case proto::OngoingOperation::kBuildingUpdate:
      res["operation"] = "config";
      res["newconfig"] = Convert (pb.building_update ().new_config ());
      break;

    default:
      LOG (FATAL) << "Unexpected ongoing operation case: " << pb.op_case ();
    }

  res["end_height"] = IntToJson (op.GetHeight () + endDelta);

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<Region> (const Region& r) const
{
  const auto& pb = r.GetProto ();

  Json::Value res(Json::objectValue);
  res["id"] = r.GetId ();

  Json::Value prospection(Json::objectValue);
  if (pb.has_prospecting_character ())
    prospection["inprogress"] = IntToJson (pb.prospecting_character ());
  if (pb.has_prospection ())
    {
      prospection["name"] = pb.prospection ().name ();
      prospection["height"] = pb.prospection ().height ();
    }

  if (!prospection.empty ())
    res["prospection"] = prospection;

  if (pb.has_prospection ())
    {
      Json::Value resource(Json::objectValue);
      resource["type"] = pb.prospection ().resource ();
      resource["amount"] = IntToJson (r.GetResourceLeft ());

      res["resource"] = resource;
    }

  return res;
}

template <>
  Json::Value
  GameStateJson::Convert<DexTrade> (const DexTrade& t) const
{
  Json::Value res(Json::objectValue);

  res["height"] = IntToJson (t.GetHeight ());
  res["timestamp"] = IntToJson (t.GetTimestamp ());

  res["buildingid"] = IntToJson (t.GetBuilding ());
  res["item"] = t.GetItem ();

  res["quantity"] = t.GetQuantity ();
  res["price"] = t.GetPrice ();
  const QuantityProduct cost(t.GetQuantity (), t.GetPrice ());
  res["cost"] = IntToJson (cost.Extract ());

  res["seller"] = t.GetSeller ();
  res["buyer"] = t.GetBuyer ();

  return res;
}

template <typename T, typename R>
  Json::Value
  GameStateJson::ResultsAsArray (T& tbl, Database::Result<R> res) const
{
  Json::Value arr(Json::arrayValue);

  while (res.Step ())
    {
      const auto h = tbl.GetFromResult (res);
      arr.append (Convert (*h));
    }

  return arr;
}

Json::Value
GameStateJson::MoneySupply ()
{
  const auto& params = ctx.RoConfig ()->params ();
  pxd::MoneySupply ms(db);

  Amount total = 0;
  Json::Value entries(Json::objectValue);
  for (const auto& key : ms.GetValidKeys ())
    {
      if (key == "gifted" && !params.god_mode ())
        {
          CHECK_EQ (ms.Get (key), 0);
          continue;
        }

      const Amount value = ms.Get (key);
      entries[key] = IntToJson (value);
      total += value;
    }

  Json::Value burnsale(Json::arrayValue);
  Amount burnsaleAmount = ms.Get ("burnsale");
  for (int i = 0; i < params.burnsale_stages_size (); ++i)
    {
      const auto& data = params.burnsale_stages (i);
      const Amount alreadySold = std::min<Amount> (burnsaleAmount,
                                                   data.amount_sold ());
      burnsaleAmount -= alreadySold;

      Json::Value stage(Json::objectValue);
      stage["stage"] = IntToJson (i + 1);
      stage["price"] = static_cast<double> (data.price_sat ()) / COIN;
      stage["total"] = IntToJson (data.amount_sold ());
      stage["sold"] = IntToJson (alreadySold);
      stage["available"] = IntToJson (data.amount_sold () - alreadySold);

      burnsale.append (stage);
    }
  CHECK_EQ (burnsaleAmount, 0);

  Json::Value res(Json::objectValue);
  res["total"] = IntToJson (total);
  res["entries"] = entries;
  res["burnsale"] = burnsale;

  return res;
}

Json::Value
GameStateJson::PrizeStats ()
{
  ItemCounts cnt(db);

  Json::Value res(Json::objectValue);
  for (const auto& p : ctx.RoConfig ()->params ().prizes ())
    {
      Json::Value cur(Json::objectValue);
      cur["number"] = p.number ();
      cur["probability"] = p.probability ();

      const unsigned found = cnt.GetFound (p.name () + " prize");
      CHECK_LE (found, p.number ());

      cur["found"] = found;
      cur["available"] = p.number () - found;

      res[p.name ()] = cur;
    }

  return res;
}

Json::Value
GameStateJson::Accounts ()
{
  AccountsTable tbl(db);
  Json::Value res = ResultsAsArray (tbl, tbl.QueryAll ());

  /* Add in also the Cubit balances reserved in open bids.  */
  const auto reserved = orders.GetReservedCoins ();
  for (auto& entry : res)
    {
      const auto& nmVal = entry["name"];
      CHECK (nmVal.isString ());
      const auto mit = reserved.find (nmVal.asString ());

      Amount cur;
      if (mit == reserved.end ())
        cur = 0;
      else
        cur = mit->second;

      auto& bal = entry["balance"];
      CHECK (bal.isObject ());
      bal["reserved"] = IntToJson (cur);
      bal["total"] = IntToJson (cur + bal["available"].asInt64 ());
    }

  return res;
}

Json::Value
GameStateJson::Buildings ()
{
  BuildingsTable tbl(db);
  return ResultsAsArray (tbl, tbl.QueryAll ());
}

Json::Value
GameStateJson::Characters ()
{
  CharacterTable tbl(db);
  return ResultsAsArray (tbl, tbl.QueryAll ());
}

Json::Value
GameStateJson::GroundLoot ()
{
  GroundLootTable tbl(db);
  return ResultsAsArray (tbl, tbl.QueryNonEmpty ());
}

Json::Value
GameStateJson::OngoingOperations ()
{
  OngoingsTable tbl(db);
  return ResultsAsArray (tbl, tbl.QueryAll ());
}

Json::Value
GameStateJson::Regions (const unsigned h)
{
  RegionsTable tbl(db, RegionsTable::HEIGHT_READONLY);
  return ResultsAsArray (tbl, tbl.QueryModifiedSince (h));
}

Json::Value
GameStateJson::TradeHistory (const std::string& item,
                             const Database::IdT building)
{
  DexHistoryTable tbl(db);
  return ResultsAsArray (tbl, tbl.QueryForItem (item, building));
}

Json::Value
GameStateJson::FullState ()
{
  Json::Value res(Json::objectValue);

  res["accounts"] = Accounts ();
  res["buildings"] = Buildings ();
  res["characters"] = Characters ();
  res["groundloot"] = GroundLoot ();
  res["ongoings"] = OngoingOperations ();
  res["moneysupply"] = MoneySupply ();
  res["regions"] = Regions (0);
  res["prizes"] = PrizeStats ();

  return res;
}

Json::Value
GameStateJson::BootstrapData ()
{
  Json::Value res(Json::objectValue);
  res["regions"] = Regions (0);

  return res;
}

} // namespace pxd
