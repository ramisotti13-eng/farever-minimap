-- ==============================================================
-- item-tracker-by-dpiza.lua
-- Submitted by dpiza
-- Tested against farever-mod v1.2.3+
-- License: MIT
--
-- Track a configurable list of items and show bag counts in a
-- compact plugin window.
--
-- Requires farever-mod v1.2.1+ for farever.player.inventory().
-- and it.stack per entry (total quantity in that stack).
--
-- Item names resolve via embedded DB_items (FareverDB data, same
-- table as item-finder-by-iskrumpie).
--
-- v1.0.1 (2026-07-17):
--   + Optional currency display via farever.player.currencies() (v1.2.3+)
--
-- v1.0.0 (2026-07-17):
--   + Live bag counts via farever.player.inventory() (stack-aware on v1.2.3+)
--   + Configurable tracked-item list persisted via farever.store
--   + Name-only editor; ids resolved from embedded DB_items (874 items)
--   + Prefix name suggestions while adding items
--   + Defaults: Ephemeral Heart, Kobold Swiss
-- ==============================================================

-- ── embedded data ─────────────────────────────────────────────────────────────
-- FareverDB item ids and display names (same DB_items set as item-finder).
local DB_items = {
  ["ActivityLoot"]={n="ActivityLoot",r="Common",t="Misc",sell=1},
  ["Agate"]={n="Agate",r="Uncommon",t="CraftingComponent",sell=20},
  ["AlchemistEssence_Z1"]={n="Fresh Essence",r="Rare",t="CraftingComponent",sell=1},
  ["Alloy_Z1"]={n="Glittering Alloy",r="Rare",t="CraftingComponent",sell=1},
  ["Alloy_Z2"]={n="Shimmering Alloy",r="Rare",t="CraftingComponent",sell=1},
  ["Amber"]={n="Amber",r="Uncommon",t="CraftingComponent",sell=10},
  ["AncientThymePetal"]={n="Ancient Thyme Petal",r="Common",t="CraftingComponent",sell=7},
  ["AttunedCutBeryl"]={n="Attuned Cut Beryl",r="Uncommon",t="AugmentJeweller",sell=1},
  ["AttunedCutRuby"]={n="Attuned Cut Ruby",r="Uncommon",t="AugmentJeweller",sell=1},
  ["Axe_Boomerang"]={n="Cheese Moon",r="Rare",t="Axe",sell=1},
  ["Back_RBee_AssCle"]={n="Flight of the Rumblebee",r="Rare",t="Back",sell=1},
  ["Back_RBee_FigAss"]={n="Reflective Vest of the Hive Worker",r="Rare",t="Back",sell=1},
  ["Back_RBee_FigWiz_Craft"]={n="Beekeeper's Scarf",r="Rare",t="Back",sell=1},
  ["Back_RBee_Wiz"]={n="Back_RBee_Wiz",r="Rare",t="Back",sell=1},
  ["Back_RCrimson_AssCle_Craft"]={n="Cloak of Rising Twilight",r="Rare",t="Back",sell=1},
  ["Back_RCrimson_AssWiz"]={n="Diamond of the Order",r="Rare",t="Back",sell=1},
  ["Back_RCrimson_Fig"]={n="Crimson Wings",r="Rare",t="Back",sell=1},
  ["Back_RCrimson_WizCle"]={n="Sacrificial Cape",r="Rare",t="Back",sell=1},
  ["Back_RKobold_Ass"]={n="Reversible Bib of the Cheese Taster",r="Rare",t="Back",sell=1},
  ["Back_RKobold_Cle"]={n="Mystical Placemat of the Gourmet",r="Rare",t="Back",sell=1},
  ["Back_RKobold_FigWiz"]={n="Brie von de Cape",r="Rare",t="Back",sell=1},
  ["Back_RManfish_AssWiz"]={n="Cloak of the Third Wave Apostle",r="Rare",t="Back",sell=1},
  ["Back_RManfish_FigCle"]={n="Jenny Hanibal's Armored Cape",r="Rare",t="Back",sell=1},
  ["Back_RManfish_WizCle"]={n="Wings of the Prophet",r="Rare",t="Back",sell=1},
  ["Back_Shop"]={n="Farseeker Cloak",r="Epic",t="Back",sell=0},
  ["Back_Z1U1_Ass"]={n="Featherlight Capelet of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U1_AssCle"]={n="Ethereal Capelet of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U1_AssWiz"]={n="Infused Capelet of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U1_Cle"]={n="Sacred Scarf of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U1_Fig"]={n="Reinforced Neck Gaiter of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U1_FigAss"]={n="Condensed Neck Gaiter of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U1_FigCle"]={n="Blessed Neck Gaiter of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U1_FigWiz"]={n="Protected Neck Gaiter of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U1_Wiz"]={n="Spellbound Scarf of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U1_WizCle"]={n="Runic Scarf of the Exile",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_Ass"]={n="Featherlight Capelet of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_AssCle"]={n="Ethereal Capelet of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_AssWiz"]={n="Infused Capelet of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_Cle"]={n="Sacred Scarf of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_Fig"]={n="Reinforced Neck Gaiter of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_FigAss"]={n="Condensed Neck Gaiter of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_FigCle"]={n="Blessed Neck Gaiter of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_FigWiz"]={n="Protected Neck Gaiter of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_Wiz"]={n="Spellbound Scarf of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z1U2_WizCle"]={n="Runic Scarf of the Trespasser",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_Ass"]={n="Featherlight Capelet of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_AssCle"]={n="Ethereal Capelet of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_AssWiz"]={n="Infused Capelet of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_Cle"]={n="Sacred Scarf of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_Fig"]={n="Reinforced Neck Gaiter of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_FigAss"]={n="Condensed Neck Gaiter of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_FigCle"]={n="Blessed Neck Gaiter of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_FigWiz"]={n="Protected Neck Gaiter of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_Wiz"]={n="Spellbound Scarf of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U1_WizCle"]={n="Runic Scarf of the Adventurer",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_Ass"]={n="Featherlight Capelet of the Nomad",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_AssCle"]={n="Ethereal Capelet of the Nomad",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_AssWiz"]={n="Back_Z2U2_AssWiz",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_Cle"]={n="Sacred Scarf of the Nomad",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_Fig"]={n="Reinforced Neck Gaiter of the Nomad",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_FigAss"]={n="Condensed Neck Gaiter of the Nomad",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_FigCle"]={n="Blessed Neck Gaiter of the Nomad",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_FigWiz"]={n="Protected Neck Gaiter of the Nomad",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_Wiz"]={n="Spellbound Scarf of the Nomad",r="Uncommon",t="Back",sell=1},
  ["Back_Z2U2_WizCle"]={n="Runic Scarf of the Nomad",r="Uncommon",t="Back",sell=1},
  ["Bag_Z2"]={n="Cotton Bag",r="Uncommon",t="Bag",sell=1},
  ["Beryl"]={n="Beryl",r="Uncommon",t="CraftingComponent",sell=10},
  ["Blood_Z1"]={n="Fresh Blood",r="Uncommon",t="CraftingComponent",sell=5},
  ["BoarMeat_Z1"]={n="Boar Meat",r="Common",t="Food",sell=2},
  ["Book_Start"]={n="Apprentice's Grimoire",r="Uncommon",t="Book",sell=1},
  ["Book_WaterOrbs"]={n="Book of Mi'Mizan",r="Rare",t="Book",sell=1},
  ["Bow_BigGame"]={n="Horns of the Wind",r="Rare",t="Bow",sell=1},
  ["Bow_Craft"]={n="Credence",r="Rare",t="Bow",sell=1},
  ["BrightVoidOrb"]={n="Bright Void Orb",r="Rare",t="CraftingComponent",sell=1},
  ["BronzeIngot"]={n="Bronze Ingot",r="Uncommon",t="CraftingComponent",sell=1},
  ["CP_Z1"]={n="Completed Package",r="Uncommon",t="CompletedPackage",sell=10},
  ["Cheese_Z1"]={n="Kobold Swiss",r="Common",t="Food",sell=3},
  ["Cheese_Z2"]={n="Ramburg Bleu",r="Common",t="Food",sell=5},
  ["Chest_C_BaseClothes"]={n="White Shirt",r="Common",t="Chest",sell=1},
  ["Chest_RBee_AssCle"]={n="Emblem of Radiant Nectar",r="Rare",t="Chest",sell=1},
  ["Chest_RBee_AssWiz_Craft"]={n="Splendid Wingsvest",r="Rare",t="Chest",sell=1},
  ["Chest_RBee_Fig"]={n="Whirring Gem of Apix",r="Rare",t="Chest",sell=1},
  ["Chest_RBee_WizCle"]={n="Casual Clothes of the Pollincess",r="Rare",t="Chest",sell=1},
  ["Chest_RCrimson_Ass"]={n="Gambeson of the Flying Ram",r="Rare",t="Chest",sell=1},
  ["Chest_RCrimson_FigCle"]={n="Breastplate of Recklessness",r="Rare",t="Chest",sell=1},
  ["Chest_RCrimson_Fig_Craft"]={n="Scarlet Breastplate",r="Rare",t="Chest",sell=1},
  ["Chest_RCrimson_Wiz"]={n="Krisomal's Golden Fleece",r="Rare",t="Chest",sell=1},
  ["Chest_RCrimson_WizCle_Craft"]={n="Sacrificial Vestments",r="Rare",t="Chest",sell=1},
  ["Chest_RKobold_AssWiz"]={n="Spirit of the Spelunker",r="Rare",t="Chest",sell=1},
  ["Chest_RKobold_Cle"]={n="Goldilock's Thrice Latched Jacket",r="Rare",t="Chest",sell=1},
  ["Chest_RKobold_FigAss"]={n="Cantal Goya's Breastplate",r="Rare",t="Chest",sell=1},
  ["Chest_RManfish_AssCle"]={n="Jacket of the Last Pirate",r="Rare",t="Chest",sell=1},
  ["Chest_RManfish_Cle"]={n="Charm of the Fisher King",r="Rare",t="Chest",sell=1},
  ["Chest_RManfish_FigWiz"]={n="Fin Armor",r="Rare",t="Chest",sell=1},
  ["Chest_Starter_Ass"]={n="Hoodlum's Doublet",r="Common",t="Chest",sell=1},
  ["Chest_Starter_Cle"]={n="Novitiate's Robe",r="Common",t="Chest",sell=1},
  ["Chest_Starter_Fig"]={n="Squire's Brigandine",r="Common",t="Chest",sell=1},
  ["Chest_Starter_Wiz"]={n="Apprentice's Tunic",r="Common",t="Chest",sell=1},
  ["Chest_Z1U1_Ass"]={n="Featherlight Doublet of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U1_AssCle"]={n="Ethereal Doublet of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U1_AssWiz"]={n="Infused Doublet of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U1_Cle"]={n="Sacred Robe of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U1_Fig"]={n="Reinforced Hauberk of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U1_FigAss"]={n="Condensed Hauberk of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U1_FigCle"]={n="Blessed Hauberk of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U1_FigWiz"]={n="Protected Hauberk of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U1_Wiz"]={n="Spellbound Robe of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U1_WizCle"]={n="Runic Robe of the Exile",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_Ass"]={n="Featherlight Doublet of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_AssCle"]={n="Ethereal Doublet of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_AssWiz"]={n="Infused Doublet of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_Cle"]={n="Sacred Robe of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_Fig"]={n="Reinforced Hauberk of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_FigAss"]={n="Condensed Hauberk of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_FigCle"]={n="Blessed Hauberk of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_FigWiz"]={n="Protected Hauberk of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_Wiz"]={n="Spellbound Robe of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z1U2_WizCle"]={n="Runic Robe of the Trespasser",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_Ass"]={n="Featherlight Jacket of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_AssCle"]={n="Ethereal Jacket of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_AssWiz"]={n="Infused Jacket of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_Cle"]={n="Sacred Vest of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_Fig"]={n="Reinforced Hauberk of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_FigAss"]={n="Condensed Hauberk of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_FigCle"]={n="Blessed Hauberk of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_FigWiz"]={n="Protected Hauberk of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_Wiz"]={n="Spellbound Vest of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U1_WizCle"]={n="Runic Vest of the Adventurer",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_Ass"]={n="Featherlight Jacket of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_AssCle"]={n="Ethereal Jacket of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_AssWiz"]={n="Infused Jacket of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_Cle"]={n="Sacred Vest of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_Fig"]={n="Reinforced Hauberk of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_FigAss"]={n="Condensed Hauberk of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_FigCle"]={n="Blessed Hauberk of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_FigWiz"]={n="Protected Hauberk of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_Wiz"]={n="Spellbound Vest of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["Chest_Z2U2_WizCle"]={n="Runic Vest of the Nomad",r="Uncommon",t="Chest",sell=1},
  ["ChippedTusk"]={n="Chipped Tusk",r="Uncommon",t="CraftingComponent",sell=7},
  ["Cloth_Z1"]={n="Linen Cloth",r="Common",t="Cloth",sell=3},
  ["Coal"]={n="Coal",r="Uncommon",t="CraftingComponent",sell=6},
  ["Cook_1"]={n="Wild Boar Stew",r="Common",t="Food",sell=1},
  ["Cook_10"]={n="Skover Root Soup",r="Common",t="Food",sell=1},
  ["Cook_11"]={n="Beggar's Garbure",r="Common",t="Food",sell=1},
  ["Cook_12"]={n="Pumpkin Pie",r="Common",t="Food",sell=1},
  ["Cook_13"]={n="Beehive Lozenge",r="Common",t="Food",sell=1},
  ["Cook_14"]={n="Ramburg Fondue",r="Common",t="Food",sell=1},
  ["Cook_15"]={n="Pike Cooked in Foil",r="Common",t="Food",sell=1},
  ["Cook_16"]={n="Sea Omelette",r="Common",t="Food",sell=1},
  ["Cook_2"]={n="Grilled Wolf Chops",r="Common",t="Food",sell=1},
  ["Cook_3"]={n="Crab Goulash",r="Common",t="Food",sell=1},
  ["Cook_4"]={n="Melted Herbed Cheese",r="Common",t="Food",sell=1},
  ["Cook_5"]={n="Grilled Mackerel",r="Common",t="Food",sell=1},
  ["Cook_6"]={n="Skunk Stew",r="Common",t="Food",sell=1},
  ["Cook_7"]={n="Flavored Candies",r="Common",t="Food",sell=1},
  ["Cook_8"]={n="Mixed Salad",r="Common",t="Food",sell=1},
  ["Cook_9"]={n="Smoked Coyote",r="Common",t="Food",sell=1},
  ["CopperIngot"]={n="Copper Ingot",r="Uncommon",t="CraftingComponent",sell=1},
  ["CopperOre"]={n="Copper Ore",r="Common",t="Ore",sell=3},
  ["CopperProspecting"]={n="Copper Prospecting",r="Common",t="Prospecting",sell=0},
  ["CopperSetting"]={n="Copper Setting",r="Uncommon",t="CraftingComponent",sell=1},
  ["CoyoteMeat"]={n="Coyote Meat",r="Common",t="Food",sell=5},
  ["CrabEgg"]={n="Crab Egg",r="Common",t="Food",sell=6},
  ["CrabMeat_Z1"]={n="Fleshy Claw",r="Common",t="Food",sell=2},
  ["CraftPoint"]={n="Craft point",r="Common",t="Currency",sell=1},
  ["Crescent_FlowerSpiral"]={n="Thornlace",r="Rare",t="Crescent",sell=1},
  ["Crystal_Z2"]={n="Glittering Essence",r="Epic",t="CraftingComponent",sell=1},
  ["CutAgate"]={n="Cut Agate",r="Uncommon",t="CraftingComponent",sell=1},
  ["CutAmber"]={n="Cut Amber",r="Uncommon",t="CraftingComponent",sell=1},
  ["CutBeryl"]={n="Cut Beryl",r="Uncommon",t="CraftingComponent",sell=1},
  ["CutMalachite"]={n="Cut Malachite",r="Rare",t="CraftingComponent",sell=1},
  ["CutRuby"]={n="Cut Ruby",r="Uncommon",t="CraftingComponent",sell=1},
  ["CutStone"]={n="Cracked Cut Stone",r="Uncommon",t="CraftingComponent",sell=1},
  ["DA_Water"]={n="Iron Fins of the Leviathan",r="Rare",t="DualAxes",sell=1},
  ["DM_Multispin"]={n="Twin Pillars of Justice",r="Rare",t="DualMaces",sell=1},
  ["DS_Z1RBee_AssWiz"]={n="Wingsabers",r="Rare",t="DualSwords",sell=1},
  ["Daggers_DuplicatePoison"]={n="Twin Fangs of Ratsar",r="Rare",t="Daggers",sell=1},
  ["Daggers_Start"]={n="Rusty Knives",r="Uncommon",t="Daggers",sell=1},
  ["DivinedCopperPlate"]={n="Divine Bronze Plate",r="Uncommon",t="AugmentBlacksmith",sell=1},
  ["DivinedEmbroidery"]={n="Divine Soft Embroidery",r="Uncommon",t="AugmentOutfitter",sell=1},
  ["DrippingDebris"]={n="Dripping Debris",r="Common",t="Usable",sell=1},
  ["ElixirOfAbundance"]={n="Elixir of Abundance",r="Common",t="Elixir",sell=1},
  ["ElixirOfArmor_Z2"]={n="Elixir of Armor",r="Common",t="Elixir",sell=1},
  ["ElixirOfDexterity_Z2"]={n="Elixir of Dexterity",r="Common",t="Elixir",sell=1},
  ["ElixirOfFaith_Z2"]={n="Elixir of Faith",r="Common",t="Elixir",sell=1},
  ["ElixirOfIntelligence_Z2"]={n="Elixir of Intelligence",r="Common",t="Elixir",sell=1},
  ["ElixirOfMinorDexterity"]={n="Elixir of Minor Dexterity",r="Common",t="Elixir",sell=1},
  ["ElixirOfMinorFaith"]={n="Elixir of Minor Faith",r="Common",t="Elixir",sell=1},
  ["ElixirOfMinorIntelligence"]={n="Elixir of Minor Intelligence",r="Common",t="Elixir",sell=1},
  ["ElixirOfMinorStrength"]={n="Elixir of Minor Strength",r="Common",t="Elixir",sell=1},
  ["ElixirOfStrength_Z2"]={n="Elixir of Strength",r="Common",t="Elixir",sell=1},
  ["ElixirofMinorArmor"]={n="Elixir of Minor Armor",r="Common",t="Elixir",sell=1},
  ["EnchantConcentrate_Z1"]={n="Bright Spark Concentrate",r="Rare",t="CraftingComponent",sell=1},
  ["EnchantPaper_Z1"]={n="Bright Enchanted Paper",r="Rare",t="CraftingComponent",sell=1},
  ["EnchantPowder_Z1"]={n="Purified Bright Powder",r="Uncommon",t="CraftingComponent",sell=1},
  ["Essence_Z2"]={n="Glittering Spark",r="Legendary",t="CraftingComponent",sell=1},
  ["Experience"]={n="Experience",r="Common",t="Currency",sell=0},
  ["Eye_Z1"]={n="Eyestalk",r="Common",t="CraftingComponent",sell=2},
  ["Fang_Z1"]={n="Small Fang",r="Common",t="CraftingComponent",sell=2},
  ["FastSwimPotion"]={n="Fast Swim Potion",r="Common",t="Potion",sell=1},
  ["Fat"]={n="Fat",r="Common",t="Food",sell=6},
  ["Feast"]={n="Plainswalker Feast",r="Rare",t="Food",sell=1},
  ["Feet_RBee_AssWiz"]={n="War Sabatons of Apix",r="Rare",t="Feet",sell=1},
  ["Feet_RBee_Cle"]={n="Cleo's Ethereal Blossoms",r="Rare",t="Feet",sell=1},
  ["Feet_RBee_Fig"]={n="Melain's Golden Greaves",r="Rare",t="Feet",sell=1},
  ["Feet_RBee_WizCle_Craft"]={n="Hiveriders",r="Rare",t="Feet",sell=1},
  ["Feet_RCrimson_Ass"]={n="Galoshes of Fortune",r="Rare",t="Feet",sell=1},
  ["Feet_RCrimson_FigCle"]={n="Rival Sabatons of Ironhorn",r="Rare",t="Feet",sell=1},
  ["Feet_RCrimson_Wiz"]={n="Curse of the Immortal",r="Rare",t="Feet",sell=1},
  ["Feet_RKobold_AssCle"]={n="Spelunking Shoes of Emmen Tunnel",r="Rare",t="Feet",sell=1},
  ["Feet_RKobold_FigAss"]={n="Buskins of Essential Emptiness",r="Rare",t="Feet",sell=1},
  ["Feet_RKobold_FigCle_Craft"]={n="Hot Cheesewalkers",r="Rare",t="Feet",sell=1},
  ["Feet_RKobold_WizCle"]={n="Gold Pouches",r="Rare",t="Feet",sell=1},
  ["Feet_RManfish_Ass"]={n="Bleak Shell's Knee Pads",r="Rare",t="Feet",sell=1},
  ["Feet_RManfish_AssWiz_Craft"]={n="Tidewalkers",r="Rare",t="Feet",sell=1},
  ["Feet_RManfish_FigWiz"]={n="Boots of Abyssal Essence",r="Rare",t="Feet",sell=1},
  ["Feet_RManfish_Wiz"]={n="Spare Sandals of Young Nephisto",r="Rare",t="Feet",sell=1},
  ["Feet_Starter_Ass"]={n="Hoodlum's Boots",r="Common",t="Feet",sell=1},
  ["Feet_Starter_Cle"]={n="Novitiate's Sandals",r="Common",t="Feet",sell=1},
  ["Feet_Starter_Fig"]={n="Squire's Shoes",r="Common",t="Feet",sell=1},
  ["Feet_Starter_Wiz"]={n="Apprentice's Shoes",r="Common",t="Feet",sell=1},
  ["Feet_Z1U1_Ass"]={n="Featherlight Boots of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U1_AssCle"]={n="Ethereal Boots of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U1_AssWiz"]={n="Infused Boots of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U1_Cle"]={n="Sacred Sandals of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U1_Fig"]={n="Reinforced Warboots of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U1_FigAss"]={n="Condensed Warboots of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U1_FigCle"]={n="Blessed Warboots of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U1_FigWiz"]={n="Protected Warboots of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U1_Wiz"]={n="Spellbound Sandals of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U1_WizCle"]={n="Runic Sandals of the Exile",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_Ass"]={n="Featherlight Boots of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_AssCle"]={n="Ethereal Boots of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_AssWiz"]={n="Infused Boots of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_Cle"]={n="Sacred Sandals of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_Fig"]={n="Reinforced Warboots of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_FigAss"]={n="Condensed Warboots of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_FigCle"]={n="Blessed Warboots of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_FigWiz"]={n="Protected Warboots of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_Wiz"]={n="Spellbound Sandals of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z1U2_WizCle"]={n="Runic Sandals of the Trespasser",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_Ass"]={n="Featherlight Boots of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_AssCle"]={n="Ethereal Boots of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_AssWiz"]={n="Infused Boots of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_Cle"]={n="Sacred Buskins of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_Fig"]={n="Reinforced Warboots of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_FigAss"]={n="Condensed Warboots of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_FigCle"]={n="Blessed Warboots of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_FigWiz"]={n="Protected Warboots of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_Wiz"]={n="Spellbound Buskins of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U1_WizCle"]={n="Runic Buskins of the Adventurer",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U2_Ass"]={n="Featherlight Boots of the Nomad",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U2_AssCle"]={n="Ethereal Boots of the Nomad",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U2_AssWiz"]={n="Infused Boots of the Nomad",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U2_Cle"]={n="Sacred Buskins of the Nomad",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U2_Fig"]={n="Reinforced Warboots of the Nomad",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U2_FigAss"]={n="Condensed Warboots of the Nomad",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U2_FigWiz"]={n="Protected Warboots of the Nomad",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U2_Wiz"]={n="Spellbound Buskins of the Nomad",r="Uncommon",t="Feet",sell=1},
  ["Feet_Z2U2_WizCle"]={n="Runic Buskins of the Nomad",r="Uncommon",t="Feet",sell=1},
  ["Fin_Z1"]={n="Sticky Fin",r="Common",t="CraftingComponent",sell=2},
  ["Finger_Ap"]={n="Ring of Fracture",r="Uncommon",t="GearFinger",sell=1},
  ["Finger_Cri"]={n="Ring of Precision",r="Uncommon",t="GearFinger",sell=1},
  ["Finger_Fer"]={n="Ring of Zeal",r="Uncommon",t="GearFinger",sell=1},
  ["Finger_Mp"]={n="Ring of Clarity",r="Uncommon",t="GearFinger",sell=1},
  ["Finger_Z1RCraft_Ap"]={n="Circle of Fracture",r="Rare",t="GearFinger",sell=1},
  ["Finger_Z1RCraft_Cri"]={n="Circle of Precision",r="Rare",t="GearFinger",sell=1},
  ["Finger_Z1RCraft_Fer"]={n="Circle of Zeal",r="Rare",t="GearFinger",sell=1},
  ["Finger_Z1RCraft_Mp"]={n="Circle of Clarity",r="Rare",t="GearFinger",sell=1},
  ["Finger_Z1_Vit"]={n="Ring of Vitality",r="Uncommon",t="GearFinger",sell=1},
  ["Finger_Z2RCraft_CriAP"]={n="Signet of the Fighter",r="Rare",t="GearFinger",sell=1},
  ["Finger_Z2RCraft_FerMP"]={n="Signet of the Wizard",r="Rare",t="GearFinger",sell=1},
  ["Finger_Z2_Ap"]={n="Ringlet of Fracture",r="Uncommon",t="GearFinger",sell=1},
  ["Finger_Z2_Cri"]={n="Ringlet of Precision",r="Uncommon",t="GearFinger",sell=1},
  ["Finger_Z2_Fer"]={n="Ringlet of Zeal",r="Uncommon",t="GearFinger",sell=1},
  ["Finger_Z2_Mp"]={n="Ringlet of Clarity",r="Uncommon",t="GearFinger",sell=1},
  ["FishermanSauce"]={n="Fisherman's Sauce",r="Uncommon",t="CraftingComponent",sell=1},
  ["Fists_LightMonk"]={n="Ramulus & Ramus",r="Rare",t="Fists",sell=1},
  ["Fists_WaterUppecut"]={n="Clawdius",r="Rare",t="Fists",sell=1},
  ["FormulaFeetArmorPen_Z2"]={n="Magic Formula: Armor Penetration",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaFeetArmor_Z2"]={n="Magic Formula: Armor",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaFeetCritical_Z2"]={n="Magic Formula: Critical Chance",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaFeetFervor_Z2"]={n="Magic Formula: Fervor",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaFeetMagicPen_Z2"]={n="Magic Formula: Magic Penetration",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaFeetMinorArmor"]={n="Magic Formula: Minor Armor",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaFeetMinorArmorPen"]={n="Magic Formula: Minor Armor Penetration",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaFeetMinorCritical"]={n="Magic Formula: Minor Critical Chance",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaFeetMinorFervor"]={n="Magic Formula: Minor Fervor",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaFeetMinorMagicPen"]={n="Magic Formula: Minor Magic Penetration",r="Uncommon",t="AugmentEnchantFeet",sell=1},
  ["FormulaHandsDexterity_Z2"]={n="Magic Formula: Dexterity",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaHandsFaith_Z2"]={n="Magic Formula: Faith",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaHandsIntelligence_Z2"]={n="Magic Formula: Intelligence",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaHandsMinorDexterity"]={n="Magic Formula: Minor Dexterity",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaHandsMinorFaith"]={n="Magic Formula: Minor Faith",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaHandsMinorIntelligence"]={n="Magic Formula: Minor Intelligence",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaHandsMinorStrength"]={n="Magic Formula: Minor Strength",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaHandsMinorVitality"]={n="Magic Formula: Minor Vitality",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaHandsStrength_Z2"]={n="Magic Formula: Strength",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaHandsVitality_Z2"]={n="Magic Formula: Vitality",r="Uncommon",t="AugmentEnchantHands",sell=1},
  ["FormulaWeaponDevote"]={n="Magic Formula: Devote",r="Rare",t="AugmentEnchantWeapon",sell=1},
  ["FormulaWeaponFlamingWeapon"]={n="Magic Formula: Flaming Weapon",r="Rare",t="AugmentEnchantWeapon",sell=1},
  ["FormulaWeaponSparkHarvesting"]={n="Magic Formula: Spark Harvesting",r="Rare",t="AugmentEnchantWeapon",sell=1},
  ["FormulaWeaponZealot"]={n="Magic Formula: Zealot",r="Rare",t="AugmentEnchantWeapon",sell=1},
  ["FragmentOfAir"]={n="Fragment of Aoyl",r="Uncommon",t="CraftingComponent",sell=25},
  ["FragmentOfEarth"]={n="Fragment of Kyre",r="Uncommon",t="CraftingComponent",sell=25},
  ["FragmentOfFire"]={n="Fragment of Pyrh",r="Uncommon",t="CraftingComponent",sell=25},
  ["FragmentOfNature"]={n="Fragment of Mely",r="Uncommon",t="CraftingComponent",sell=25},
  ["FragmentOfWater"]={n="Fragment of Naya",r="Uncommon",t="CraftingComponent",sell=25},
  ["Fragments_Z1"]={n="Bright Fragments",r="Rare",t="CraftingComponent",sell=15},
  ["FreshFlowerPowder"]={n="Fresh Flower Powder",r="Uncommon",t="CraftingComponent",sell=1},
  ["GA_Craft"]={n="Judgement",r="Rare",t="GreatAxe",sell=1},
  ["GM_MassGrab"]={n="Pocket Hive",r="Rare",t="GreatMace",sell=1},
  ["GS_Z1Mines_Fig"]={n="Martyr of Enripit",r="Rare",t="GreatSword",sell=1},
  ["GildedCutBeryl"]={n="Gilded Cut Beryl",r="Uncommon",t="AugmentJeweller",sell=1},
  ["GildedCutRuby"]={n="Gilded Cut Ruby",r="Uncommon",t="AugmentJeweller",sell=1},
  ["Glider_Bat_BlackWhite"]={n="Navelian Bat",r="Epic",t="GearGlider",sell=500},
  ["Glider_Bat_Brown"]={n="Enripian Bat",r="Epic",t="GearGlider",sell=500},
  ["Glider_Bat_Burning"]={n="Incandescent Bat",r="Epic",t="GearGlider",sell=0},
  ["Glider_Bat_Demon"]={n="Niflelian Bat",r="Epic",t="GearGlider",sell=0},
  ["Glider_Bat_Grey"]={n="Skoverian Bat",r="Epic",t="GearGlider",sell=0},
  ["Glider_Bat_WhiteBlack"]={n="Krisomalese Bat",r="Epic",t="GearGlider",sell=500},
  ["Glider_Butterfly_Blue"]={n="Meropsian Moth",r="Epic",t="GearGlider",sell=0},
  ["Glider_Butterfly_Demon"]={n="Niflelian Moth",r="Epic",t="GearGlider",sell=0},
  ["Glider_Butterfly_EA_Spark"]={n="Sparkling Proto-moth",r="Epic",t="GearGlider",sell=0},
  ["Glider_Butterfly_Green"]={n="Eksodean Moth",r="Epic",t="GearGlider",sell=0},
  ["Glider_Butterfly_Orange"]={n="Antelimbian Moth",r="Epic",t="GearGlider",sell=0},
  ["Glider_Butterfly_Pink"]={n="Pink Moth",r="Epic",t="GearGlider",sell=0},
  ["Glider_Butterfly_Yellow"]={n="Semeruian Moth",r="Epic",t="GearGlider",sell=500},
  ["Glider_Dragon_Blue"]={n="Meropsian Dragoon",r="Epic",t="GearGlider",sell=0},
  ["Glider_Dragon_BlueGreen"]={n="Eksodean Dragoon",r="Epic",t="GearGlider",sell=0},
  ["Glider_Dragon_Demon"]={n="Niflelian Dragoon",r="Epic",t="GearGlider",sell=0},
  ["Glider_Dragon_Lava"]={n="Ebral Dragoon",r="Epic",t="GearGlider",sell=500},
  ["Glider_Dragon_Orange"]={n="Antelimbian Dragoon",r="Epic",t="GearGlider",sell=0},
  ["Glider_Dragon_Pink"]={n="Pink Dragoon",r="Epic",t="GearGlider",sell=0},
  ["Glider_Dragon_Yellow"]={n="Semeruian Dragoon",r="Epic",t="GearGlider",sell=0},
  ["Glider_Falcon_Black"]={n="Navelian Featherbeak",r="Epic",t="GearGlider",sell=0},
  ["Glider_Falcon_Blue"]={n="Meropsian Featherbeak",r="Epic",t="GearGlider",sell=0},
  ["Glider_Falcon_Brown"]={n="Enripian Featherbeak",r="Epic",t="GearGlider",sell=0},
  ["Glider_Falcon_Green"]={n="Eksodean Featherbeak",r="Epic",t="GearGlider",sell=0},
  ["Glider_Falcon_Grey"]={n="Skoverian Featherbeak",r="Epic",t="GearGlider",sell=0},
  ["Glider_Falcon_Red"]={n="Egheretrian Featherbeak",r="Epic",t="GearGlider",sell=0},
  ["Glider_Falcon_White"]={n="Obralian Featherbeak",r="Epic",t="GearGlider",sell=0},
  ["Glider_FlyingFish_Acid"]={n="Acidic Wingfish",r="Epic",t="GearGlider",sell=500},
  ["Glider_FlyingFish_BlueGreen"]={n="Meropsian Wingfish",r="Epic",t="GearGlider",sell=500},
  ["Glider_FlyingFish_Demon"]={n="Niflelian Wingfish",r="Epic",t="GearGlider",sell=0},
  ["Glider_FlyingFish_Green"]={n="Skoverian Wingfish",r="Epic",t="GearGlider",sell=0},
  ["Glider_FlyingFish_Orange"]={n="Antelimbian Wingfish",r="Epic",t="GearGlider",sell=500},
  ["Glider_FlyingFish_Red"]={n="Egheretrian Wingfish",r="Epic",t="GearGlider",sell=0},
  ["Glider_Owl_BlackWarm"]={n="Krisomalese Owl",r="Epic",t="GearGlider",sell=0},
  ["Glider_Owl_BlueGrey"]={n="Zerzurian Owl",r="Epic",t="GearGlider",sell=500},
  ["Glider_Owl_Brown"]={n="Enripian Owl",r="Epic",t="GearGlider",sell=0},
  ["Glider_Owl_Brown02"]={n="Almazean Owl",r="Epic",t="GearGlider",sell=500},
  ["Glider_Owl_Grey"]={n="Azuramean Owl",r="Epic",t="GearGlider",sell=0},
  ["Glider_Owl_PurpleGrey"]={n="Ponogian Owl",r="Epic",t="GearGlider",sell=500},
  ["Glider_Raccoon_BlackBrown"]={n="Krisomalese Raccoon",r="Epic",t="GearGlider",sell=0},
  ["Glider_Raccoon_BlueGrey"]={n="Zerzurian Raccoon",r="Epic",t="GearGlider",sell=500},
  ["Glider_Raccoon_Brown01"]={n="Semeruian Raccoon",r="Epic",t="GearGlider",sell=500},
  ["Glider_Raccoon_Brown02"]={n="Enripian Raccoon",r="Epic",t="GearGlider",sell=500},
  ["Glider_Raccoon_Grey"]={n="Skoverial Raccoon",r="Epic",t="GearGlider",sell=0},
  ["Glider_Raccoon_Orange"]={n="Antelimbian Raccoon",r="Epic",t="GearGlider",sell=0},
  ["Glider_Sprout_Brown"]={n="Skoverian Seedbird",r="Epic",t="GearGlider",sell=0},
  ["Glider_Sprout_Green"]={n="Eksodean Seedbird",r="Epic",t="GearGlider",sell=500},
  ["Glider_Sprout_Orange"]={n="Antelimbian Seedbird",r="Epic",t="GearGlider",sell=0},
  ["Glider_Sprout_Red"]={n="Egheretrian Seedbird",r="Epic",t="GearGlider",sell=0},
  ["Glider_Sprout_WhitePink"]={n="Navelian Seedbird",r="Epic",t="GearGlider",sell=0},
  ["Glider_Sprout_Yellow"]={n="Semeruian Seedbird",r="Epic",t="GearGlider",sell=0},
  ["GlossyChitin"]={n="Glossy Chitin",r="Uncommon",t="CraftingComponent",sell=10},
  ["Gold"]={n="Gold",r="Common",t="Currency",sell=1},
  ["GracefulCopperPlate"]={n="Graceful Bronze Plate",r="Uncommon",t="AugmentBlacksmith",sell=1},
  ["GracefulEmbroidery"]={n="Graceful Soft Embroidery",r="Uncommon",t="AugmentOutfitter",sell=1},
  ["Halos_Demon"]={n="Halos_Demon",r="Rare",t="Halos",sell=1},
  ["Halos_Totem"]={n="Ghost Clams of the Low Tide",r="Rare",t="Halos",sell=1},
  ["Hammer"]={n="Worn Hammer",r="Common",t="ToolBlacksmith",sell=1},
  ["Hands_RBee_AssWiz"]={n="Vambraces of the Swarm",r="Rare",t="Hands",sell=1},
  ["Hands_RBee_Cle"]={n="Palmaryllis",r="Rare",t="Hands",sell=1},
  ["Hands_RBee_FigAss"]={n="Gauntlets of the Royal Guard",r="Rare",t="Hands",sell=1},
  ["Hands_RCrimson_Ass"]={n="Touch of Menas the Thaumaturge",r="Rare",t="Hands",sell=1},
  ["Hands_RCrimson_Fig"]={n="Unholy Crimson Gloves",r="Rare",t="Hands",sell=1},
  ["Hands_RCrimson_Wiz"]={n="Robin Hoof's Archery Gloves",r="Rare",t="Hands",sell=1},
  ["Hands_RKobold_AssCle"]={n="Gloves of Ninkilim the Envoy",r="Rare",t="Hands",sell=1},
  ["Hands_RKobold_Cle_Craft"]={n="Gloves of the Cheesomancer",r="Rare",t="Hands",sell=1},
  ["Hands_RKobold_FigWiz"]={n="Diskobold's Discus Throw Gloves",r="Rare",t="Hands",sell=1},
  ["Hands_RKobold_WizCle"]={n="Vows of Prosperity",r="Rare",t="Hands",sell=1},
  ["Hands_RManfish_Ass"]={n="Burden of the Abyss",r="Rare",t="Hands",sell=1},
  ["Hands_RManfish_FigAss_Craft"]={n="Handguards of the Deep Sea",r="Rare",t="Hands",sell=1},
  ["Hands_RManfish_FigCle"]={n="Palm of the Lagoon",r="Rare",t="Hands",sell=1},
  ["Hands_RManfish_Wiz"]={n="Ceremonial Siren Gloves",r="Rare",t="Hands",sell=1},
  ["Hands_Z1U1_Ass"]={n="Featherlight Hand Wraps of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U1_AssCle"]={n="Ethereal Hand Wraps of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U1_AssWiz"]={n="Infused Hand Wraps of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U1_Cle"]={n="Sacred Mittens of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U1_Fig"]={n="Reinforced Gauntlets of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U1_FigAss"]={n="Condensed Gauntlets of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U1_FigCle"]={n="Blessed Gauntlets of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U1_FigWiz"]={n="Protected Gauntlets of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U1_Wiz"]={n="Spellbound Mittens of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U1_WizCle"]={n="Runic Mittens of the Exile",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_Ass"]={n="Featherlight Hand Wraps of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_AssCle"]={n="Ethereal Hand Wraps of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_AssWiz"]={n="Infused Hand Wraps of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_Cle"]={n="Sacred Mittens of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_Fig"]={n="Reinforced Gauntlets of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_FigAss"]={n="Condensed Gauntlets of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_FigCle"]={n="Blessed Gauntlets of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_FigWiz"]={n="Protected Gauntlets of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_Wiz"]={n="Spellbound Mittens of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z1U2_WizCle"]={n="Runic Mittens of the Trespasser",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_Ass"]={n="Featherlight Armlets of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_AssCle"]={n="Ethereal Armlets of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_AssWiz"]={n="Infused Armlets of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_Cle"]={n="Sacred Wrists of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_Fig"]={n="Reinforced Gauntlets of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_FigAss"]={n="Condensed Gauntlets of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_FigCle"]={n="Blessed Gauntlets of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_FigWiz"]={n="Protected Gauntlets of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_Wiz"]={n="Spellbound Wrists of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U1_WizCle"]={n="Runic Wrists of the Adventurer",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_Ass"]={n="Featherlight Armlets of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_AssCle"]={n="Ethereal Armlets of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_AssWiz"]={n="Infused Armlets of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_Cle"]={n="Sacred Wrists of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_Fig"]={n="Reinforced Gauntlets of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_FigAss"]={n="Condensed Gauntlets of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_FigCle"]={n="Blessed Gauntlets of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_FigWiz"]={n="Protected Gauntlets of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_Wiz"]={n="Spellbound Wrists of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Hands_Z2U2_WizCle"]={n="Runic Wrists of the Nomad",r="Uncommon",t="Hands",sell=1},
  ["Head_RBee_AssWiz"]={n="Scout Antennae",r="Rare",t="Head",sell=1},
  ["Head_RBee_FigWiz"]={n="Royal Chamber Helmet",r="Rare",t="Head",sell=1},
  ["Head_RBee_WizCle"]={n="Vision of the Beekeeper",r="Rare",t="Head",sell=1},
  ["Head_RCrimson_AssCle"]={n="High-Ranking Official's Hat",r="Rare",t="Head",sell=1},
  ["Head_RCrimson_Cle"]={n="Clerical Veil",r="Rare",t="Head",sell=1},
  ["Head_RCrimson_FigAss"]={n="Ram Faceshield",r="Rare",t="Head",sell=1},
  ["Head_RKobold_AssWiz"]={n="Vision of the Cheeseslicer",r="Rare",t="Head",sell=1},
  ["Head_RKobold_FigCle"]={n="Armored Docker Cap",r="Rare",t="Head",sell=1},
  ["Head_RKobold_WizCle"]={n="Dust Scarf",r="Rare",t="Head",sell=1},
  ["Head_RManfish_Ass"]={n="Submarine Torpedo Helmet",r="Rare",t="Head",sell=1},
  ["Head_RManfish_Fig"]={n="Crown of the Sea",r="Rare",t="Head",sell=1},
  ["Head_RManfish_Wiz"]={n="Tides Hood",r="Rare",t="Head",sell=1},
  ["Head_Shop"]={n="Farseeker Goggles",r="Epic",t="Head",sell=0},
  ["Head_Z2U1_Ass"]={n="Featherlight Headband of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U1_AssCle"]={n="Ethereal Headband of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U1_AssWiz"]={n="Infused Headband of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U1_Cle"]={n="Sacred Circlet of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U1_Fig"]={n="Reinforced Helmet of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U1_FigAss"]={n="Condensed Helmet of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U1_FigCle"]={n="Blessed Helmet of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U1_FigWiz"]={n="Protected Helmet of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U1_Wiz"]={n="Spellbound Circlet of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U1_WizCle"]={n="Runic Circlet of the Adventurer",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_Ass"]={n="Featherlight Headband of the Nomad",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_AssCle"]={n="Ethereal Headband of the Nomad",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_AssWiz"]={n="Infused Headband of the Nomad",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_Cle"]={n="Sacred Circlet of the Nomad",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_Fig"]={n="Reinforced Helmet of the Nomad",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_FigAss"]={n="Condensed Helmet of the Nomad",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_FigCle"]={n="Blessed Helmet of the Nomad",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_FigWiz"]={n="Protected Helmet of the Nomad",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_Wiz"]={n="Spellbound Circlet of the Nomad",r="Uncommon",t="Head",sell=1},
  ["Head_Z2U2_WizCle"]={n="Runic Circlet of the Nomad",r="Uncommon",t="Head",sell=1},
  ["HealthPotion"]={n="Health Potion",r="Common",t="HealthPotion",sell=1},
  ["HideboundWeave"]={n="Soft Weave",r="Rare",t="CraftingComponent",sell=1},
  ["HonedCopperPlate"]={n="Honed Bronze Plate",r="Uncommon",t="AugmentBlacksmith",sell=1},
  ["HonedEmbroidery"]={n="Honed Soft Embroidery",r="Uncommon",t="AugmentOutfitter",sell=1},
  ["Honey_Z1"]={n="Honey",r="Common",t="Food",sell=3},
  ["HunterSauce"]={n="Hunter's Sauce",r="Uncommon",t="CraftingComponent",sell=1},
  ["InfusedTusk"]={n="Infused Tusk",r="Rare",t="GearTrinket",sell=1},
  ["InvisibilityPotion"]={n="Invisibility Potion",r="Common",t="Potion",sell=1},
  ["IronIngot"]={n="Iron Ingot",r="Uncommon",t="CraftingComponent",sell=1},
  ["IronOre"]={n="Iron Ore",r="Common",t="Ore",sell=9},
  ["Knife"]={n="Worn Knife",r="Common",t="ToolCook",sell=1},
  ["KoboldMetal"]={n="Piece of Perforated Metal",r="Uncommon",t="CraftingComponent",sell=10},
  ["LP_Z1_Bee"]={n="LP_Z1_Bee",r="Uncommon",t="Package",sell=1},
  ["LP_Z1_Boar"]={n="LP_Z1_Boar",r="Uncommon",t="Package",sell=1},
  ["LP_Z1_Copper"]={n="LP_Z1_Copper",r="Uncommon",t="Package",sell=1},
  ["LP_Z1_Crab"]={n="Lost Package",r="Uncommon",t="Package",sell=1},
  ["LP_Z1_Lavendula"]={n="LP_Z1_Lavendula",r="Uncommon",t="Package",sell=1},
  ["LP_Z1_Madrigold"]={n="LP_Z1_Madrigold",r="Uncommon",t="Package",sell=1},
  ["LP_Z1_Manfish"]={n="LP_Z1_Manfish",r="Uncommon",t="Package",sell=1},
  ["LP_Z1_Wolf"]={n="LP_Z1_Wolf",r="Uncommon",t="Package",sell=1},
  ["LP_Z2_Bee"]={n="LP_Z2_Bee",r="Uncommon",t="Package",sell=1},
  ["LP_Z2_Boar"]={n="LP_Z2_Boar",r="Uncommon",t="Package",sell=1},
  ["LP_Z2_Coyote"]={n="LP_Z2_Coyote",r="Uncommon",t="Package",sell=1},
  ["LP_Z2_Kobolds"]={n="LP_Z2_Kobolds",r="Uncommon",t="Package",sell=1},
  ["LP_Z2_Manfish"]={n="LP_Z2_Manfish",r="Uncommon",t="Package",sell=1},
  ["LP_Z2_Nature"]={n="LP_Z2_Nature",r="Uncommon",t="Package",sell=1},
  ["LP_Z2_Spirits"]={n="LP_Z2_Spirits",r="Uncommon",t="Package",sell=1},
  ["LP_Z2_Sprouts"]={n="LP_Z2_Sprouts",r="Uncommon",t="Package",sell=1},
  ["LavendulaPetal"]={n="Lavendula Petal",r="Common",t="CraftingComponent",sell=5},
  ["Leather_Z1"]={n="Light Leather Strap",r="Common",t="Leather",sell=1},
  ["Legs_RBee_Ass"]={n="Silhouette of Apix",r="Rare",t="Legs",sell=1},
  ["Legs_RBee_FigCle"]={n="Flying Trousers Prototype",r="Rare",t="Legs",sell=1},
  ["Legs_RBee_WizCle"]={n="Zenobee's Breeches",r="Rare",t="Legs",sell=1},
  ["Legs_RCrimson_AssCle"]={n="Ubu's Galligaskins",r="Rare",t="Legs",sell=1},
  ["Legs_RCrimson_Cle"]={n="Entommeure's Spiritual Breeches",r="Rare",t="Legs",sell=1},
  ["Legs_RCrimson_FigWiz"]={n="Crimson Pants",r="Rare",t="Legs",sell=1},
  ["Legs_RKobold_AssWiz"]={n="Garment of the Aurock Master",r="Rare",t="Legs",sell=1},
  ["Legs_RKobold_Fig"]={n="Wrong Trousers",r="Rare",t="Legs",sell=1},
  ["Legs_RKobold_Wiz"]={n="Rat-Vachol's Patched Up Pants",r="Rare",t="Legs",sell=1},
  ["Legs_RManfish_AssCle"]={n="Nepicur's Vacation Shorts",r="Rare",t="Legs",sell=1},
  ["Legs_RManfish_Cle"]={n="High-speed Clamdiggers",r="Rare",t="Legs",sell=1},
  ["Legs_RManfish_FigAss"]={n="Vesture of the Mussel Hunter",r="Rare",t="Legs",sell=1},
  ["Legs_Starter_Ass"]={n="Hoodlum's Pants",r="Common",t="Legs",sell=1},
  ["Legs_Starter_Cle"]={n="Novitiate's Breeches",r="Common",t="Legs",sell=1},
  ["Legs_Starter_Fig"]={n="Squire's Galligaskins",r="Common",t="Legs",sell=1},
  ["Legs_Starter_Wiz"]={n="Apprentice's Chausses",r="Common",t="Legs",sell=1},
  ["Legs_Z1U1_Ass"]={n="Featherlight Trousers of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U1_AssCle"]={n="Ethereal Trousers of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U1_AssWiz"]={n="Infused Trousers of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U1_Cle"]={n="Sacred Breeches of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U1_Fig"]={n="Reinforced Galligaskins of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U1_FigAss"]={n="Condensed Galligaskins of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U1_FigCle"]={n="Blessed Galligaskins of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U1_FigWiz"]={n="Protected Galligaskins of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U1_Wiz"]={n="Spellbound Breeches of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U1_WizCle"]={n="Runic Breeches of the Exile",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_Ass"]={n="Featherlight Trousers of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_AssCle"]={n="Ethereal Trousers of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_AssWiz"]={n="Infused Trousers of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_Cle"]={n="Sacred Breeches of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_Fig"]={n="Reinforced Galligaskins of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_FigAss"]={n="Condensed Galligaskins of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_FigCle"]={n="Blessed Galligaskins of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_FigWiz"]={n="Protected Galligaskins of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_Wiz"]={n="Spellbound Breeches of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z1U2_WizCle"]={n="Runic Breeches of the Trespasser",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_Ass"]={n="Featherlight Pants of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_AssCle"]={n="Ethereal Pants of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_AssWiz"]={n="Infused Pants of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_Cle"]={n="Sacred Breeches of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_Fig"]={n="Reinforced Galligaskins of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_FigAss"]={n="Condensed Galligaskins of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_FigCle"]={n="Blessed Galligaskins of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_FigWiz"]={n="Protected Galligaskins of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_Wiz"]={n="Spellbound Breeches of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U1_WizCle"]={n="Runic Breeches of the Adventurer",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_Ass"]={n="Featherlight Pants of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_AssCle"]={n="Ethereal Pants of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_AssWiz"]={n="Infused Pants of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_Cle"]={n="Sacred Breeches of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_Fig"]={n="Reinforced Galligaskins of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_FigAss"]={n="Condensed Galligaskins of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_FigCle"]={n="Blessed Galligaskins of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_FigWiz"]={n="Protected Galligaskins of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_Wiz"]={n="Spellbound Breeches of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["Legs_Z2U2_WizCle"]={n="Runic Breeches of the Nomad",r="Uncommon",t="Legs",sell=1},
  ["LinenBag"]={n="Linen Bag",r="Uncommon",t="Bag",sell=1},
  ["LinenBolt"]={n="Bolt of Linen Cloth",r="Uncommon",t="CraftingComponent",sell=1},
  ["LongBreathPotion"]={n="Long Breath Potion",r="Common",t="Potion",sell=1},
  ["Mace_Benediction"]={n="Amon Ram, the Creator",r="Rare",t="Mace",sell=1},
  ["Mackerel"]={n="Small Mackerel",r="Common",t="Food",sell=2},
  ["MadrigoldPetal"]={n="Madrigold Petal",r="Common",t="CraftingComponent",sell=3},
  ["Malachite"]={n="Malachite",r="Rare",t="CraftingComponent",sell=25},
  ["Mastery"]={n="Rune: ::ref_mastery::",r="Epic",t="Mastery",sell=100},
  ["MinorHealingPotion"]={n="Minor Healing Potion",r="Common",t="HealthPotion",sell=1},
  ["MinorShieldPotion"]={n="Minor Shield Potion",r="Common",t="Potion",sell=1},
  ["MinorVelocityPotion"]={n="Minor Velocity Potion",r="Common",t="Potion",sell=1},
  ["Mortar"]={n="Wooden Mortar",r="Common",t="ToolAlchemist",sell=1},
  ["MoteOfAir"]={n="Mote of Aoyl",r="Common",t="CraftingComponent",sell=5},
  ["MoteOfEarth"]={n="Mote of Kyre",r="Common",t="CraftingComponent",sell=5},
  ["MoteOfFire"]={n="Mote of Pyrh",r="Common",t="CraftingComponent",sell=5},
  ["MoteOfNature"]={n="Mote of Mely",r="Common",t="CraftingComponent",sell=5},
  ["MoteOfWater"]={n="Mote of Naya",r="Common",t="CraftingComponent",sell=5},
  ["Mount_Boar_01"]={n="Meridional Hog",r="Epic",t="Mount",sell=1000},
  ["Mount_Boar_02"]={n="Jodarian Hog",r="Epic",t="Mount",sell=0},
  ["Mount_Boar_03"]={n="Alandian Hog",r="Epic",t="Mount",sell=0},
  ["Mount_Boar_04"]={n="Irukalean Hog",r="Epic",t="Mount",sell=0},
  ["Mount_Boar_05"]={n="Veilantine Hog",r="Epic",t="Mount",sell=0},
  ["Mount_Boar_06"]={n="Niflelian Hog",r="Epic",t="Mount",sell=0},
  ["Mount_Crab_Blue"]={n="Meropsian Crab",r="Epic",t="Mount",sell=1000},
  ["Mount_Crab_BlueGrey"]={n="Zerzurian Crab",r="Epic",t="Mount",sell=0},
  ["Mount_Crab_Demonic"]={n="Niflelian Crab",r="Epic",t="Mount",sell=0},
  ["Mount_Crab_Purple"]={n="Ponogian Crab",r="Epic",t="Mount",sell=0},
  ["Mount_Crab_Red"]={n="Antelimbian Crab",r="Epic",t="Mount",sell=1000},
  ["Mount_Crab_Red02"]={n="Crimson Crab",r="Epic",t="Mount",sell=0},
  ["Mount_Crab_Yellow"]={n="Semeruian Crab",r="Epic",t="Mount",sell=0},
  ["Mount_Croco_01"]={n="Eksodean Crocoboar",r="Epic",t="Mount",sell=0},
  ["Mount_Croco_02"]={n="Ruleanese Crocoboar",r="Epic",t="Mount",sell=0},
  ["Mount_Croco_03"]={n="Beltirian Crocoboar",r="Epic",t="Mount",sell=0},
  ["Mount_Croco_04"]={n="Egheretrian Crocoboar",r="Epic",t="Mount",sell=0},
  ["Mount_Croco_05"]={n="Atlanese Crocoboar",r="Epic",t="Mount",sell=0},
  ["Mount_Croco_06"]={n="Niflelian Crocoboar",r="Epic",t="Mount",sell=0},
  ["Mount_Goat_01"]={n="Ebralean Goat",r="Epic",t="Mount",sell=1000},
  ["Mount_Goat_02"]={n="Kondalese Goat",r="Epic",t="Mount",sell=1000},
  ["Mount_Goat_03"]={n="Opaline Goat",r="Epic",t="Mount",sell=1000},
  ["Mount_Goat_04"]={n="Crimson Goat",r="Epic",t="Mount",sell=0},
  ["Mount_Goat_05"]={n="Zerzurean Goat",r="Epic",t="Mount",sell=0},
  ["Mount_Goat_06"]={n="Niflelian Goat",r="Epic",t="Mount",sell=0},
  ["Mount_Hound_01"]={n="Fountrailian Hound",r="Epic",t="Mount",sell=1000},
  ["Mount_Hound_02"]={n="Skoverial Hound",r="Epic",t="Mount",sell=0},
  ["Mount_Hound_03"]={n="Antelimbian Hound",r="Epic",t="Mount",sell=0},
  ["Mount_Hound_04"]={n="Ramburgian Hound",r="Epic",t="Mount",sell=0},
  ["Mount_Hound_05"]={n="Lemian Hound",r="Epic",t="Mount",sell=0},
  ["Mount_Hound_06"]={n="Niflelian Hound",r="Epic",t="Mount",sell=0},
  ["Mount_Ladybug_Blue"]={n="Zerzurean Leggybug",r="Epic",t="Mount",sell=1000},
  ["Mount_Ladybug_DarkBlue"]={n="Alandian Leggybug",r="Epic",t="Mount",sell=0},
  ["Mount_Ladybug_Demonic"]={n="Niflelian Leggybug",r="Epic",t="Mount",sell=0},
  ["Mount_Ladybug_Green"]={n="Eksodean Leggybug",r="Epic",t="Mount",sell=0},
  ["Mount_Ladybug_Orange"]={n="Sforian Leggybug",r="Epic",t="Mount",sell=0},
  ["Mount_Ladybug_Purple"]={n="Ponogian Leggybug",r="Epic",t="Mount",sell=1000},
  ["Mount_Ladybug_Red"]={n="Nescentine Leggybug",r="Epic",t="Mount",sell=1000},
  ["Mount_Ladybug_Yellow"]={n="Antelimbian Leggybug",r="Epic",t="Mount",sell=0},
  ["Mount_Skunk_01"]={n="Primevallean Skunk",r="Epic",t="Mount",sell=1000},
  ["Mount_Skunk_02"]={n="Sperian Skunk",r="Epic",t="Mount",sell=0},
  ["Mount_Skunk_03"]={n="Crosselian Skunk",r="Epic",t="Mount",sell=0},
  ["Mount_Skunk_04"]={n="Rinuhrian Skunk",r="Epic",t="Mount",sell=1000},
  ["Mount_Skunk_05"]={n="Ponogian Skunk",r="Epic",t="Mount",sell=0},
  ["Mount_Skunk_06"]={n="Niflelian Skunk",r="Epic",t="Mount",sell=0},
  ["Mount_Wolf_01"]={n="Enripian Wolf",r="Epic",t="Mount",sell=1000},
  ["Mount_Wolf_02"]={n="Nescentine Wolf",r="Epic",t="Mount",sell=1000},
  ["Mount_Wolf_03"]={n="Krisomalese Wolf",r="Epic",t="Mount",sell=0},
  ["Mount_Wolf_04"]={n="Navelian Wolf",r="Epic",t="Mount",sell=1000},
  ["Mount_Wolf_05"]={n="Almazean Wolf",r="Epic",t="Mount",sell=1000},
  ["Mount_Wolf_06"]={n="Sforian Wolf",r="Epic",t="Mount",sell=0},
  ["MysticCopperPlate"]={n="Mystic Bronze Plate",r="Uncommon",t="AugmentBlacksmith",sell=1},
  ["MysticEmbroidery"]={n="Mystic Soft Embroidery",r="Uncommon",t="AugmentOutfitter",sell=1},
  ["Necklace_Z1RCraft"]={n="Pendant of Versatility",r="Rare",t="GearNeck",sell=1},
  ["Necklace_Z1_Ap"]={n="Amulet of Fracture",r="Uncommon",t="GearNeck",sell=1},
  ["Necklace_Z1_Cri"]={n="Amulet of Precision",r="Uncommon",t="GearNeck",sell=1},
  ["Necklace_Z1_Fer"]={n="Amulet of Zeal",r="Uncommon",t="GearNeck",sell=1},
  ["Necklace_Z1_Mp"]={n="Amulet of Clarity",r="Uncommon",t="GearNeck",sell=1},
  ["Necklace_Z1_Vit"]={n="Amulet of Vitality",r="Uncommon",t="GearNeck",sell=1},
  ["Necklace_Z2RCraft"]={n="Pendant of Adaptability",r="Rare",t="GearNeck",sell=1},
  ["Necklace_Z2_Ap"]={n="Necklace of Fracture",r="Uncommon",t="GearNeck",sell=1},
  ["Necklace_Z2_Cri"]={n="Necklace of Precision",r="Uncommon",t="GearNeck",sell=1},
  ["Necklace_Z2_Fer"]={n="Necklace of Zeal",r="Uncommon",t="GearNeck",sell=1},
  ["Necklace_Z2_Mp"]={n="Necklace of Clarity",r="Uncommon",t="GearNeck",sell=1},
  ["NepsidScale"]={n="Sharp Scale",r="Uncommon",t="CraftingComponent",sell=5},
  ["Net_Basic"]={n="Large Butterfly Net",r="Rare",t="CaptureNet",sell=150},
  ["Paper_Z1"]={n="Blank Page",r="Uncommon",t="CraftingComponent",sell=50},
  ["Particle_Z1"]={n="Bright Void Particles",r="Uncommon",t="CraftingComponent",sell=5},
  ["Pearl"]={n="Simple Pearl",r="Uncommon",t="CraftingComponent",sell=20},
  ["PhilosopherStone"]={n="Philosopher's Stone",r="Rare",t="GearTrinket",sell=1},
  ["Pickaxe"]={n="Worn Pickaxe",r="Common",t="GearPickaxe",sell=50},
  ["Pike"]={n="Pike",r="Common",t="Food",sell=5},
  ["Pliers"]={n="Worn Pliers",r="Common",t="ToolJeweller",sell=1},
  ["PrismaticFragment"]={n="Prismatic Fragment",r="Rare",t="CraftingComponent",sell=1},
  ["PrismaticPearl"]={n="Prismatic Pearl",r="Rare",t="GearTrinket",sell=1},
  ["Pumpkin"]={n="Pumpkin",r="Common",t="Food",sell=5},
  ["PurifiedHeart"]={n="Purified Heart",r="Rare",t="GearTrinket",sell=1},
  ["Ramgold"]={n="Ramgold",r="Rare",t="CraftingComponent",sell=25},
  ["RecipeGenerator"]={n="RecipeGenerator",r="Common",t="Misc",sell=1},
  ["Recipe_AttunedCutBeryl"]={n="Recipe_AttunedCutBeryl",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_AttunedCutRuby"]={n="Recipe_AttunedCutRuby",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_Cook_1"]={n="Recipe_Cook_1",r="Common",t="Recipe",sell=50},
  ["Recipe_Cook_13"]={n="Recipe_Cook_13",r="Common",t="Recipe",sell=50},
  ["Recipe_Cook_14"]={n="Recipe_Cook_14",r="Common",t="Recipe",sell=50},
  ["Recipe_Cook_15"]={n="Recipe_Cook_15",r="Common",t="Recipe",sell=50},
  ["Recipe_Cook_16"]={n="Recipe_Cook_16",r="Common",t="Recipe",sell=50},
  ["Recipe_Cook_2"]={n="Recipe_Cook_2",r="Common",t="Recipe",sell=50},
  ["Recipe_Cook_5"]={n="Recipe_Cook_5",r="Common",t="Recipe",sell=50},
  ["Recipe_Cook_7"]={n="Recipe_Cook_7",r="Common",t="Recipe",sell=50},
  ["Recipe_DivinedCopperPlate"]={n="Recipe_DivinedCopperPlate",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_DivinedEmbroidery"]={n="Recipe_DivinedEmbroidery",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_ElixirOfAbundance"]={n="Recipe_ElixirOfAbundance",r="Common",t="Recipe",sell=50},
  ["Recipe_ElixirOfArmor_Z2"]={n="Recipe_ElixirOfArmor_Z2",r="Common",t="Recipe",sell=100},
  ["Recipe_ElixirofMinorArmor"]={n="Recipe_ElixirofMinorArmor",r="Common",t="Recipe",sell=50},
  ["Recipe_FastSwimPotion"]={n="Recipe_FastSwimPotion",r="Common",t="Recipe",sell=50},
  ["Recipe_FormulaFeetArmorPen_Z2"]={n="Recipe_FormulaFeetArmorPen_Z2",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_FormulaFeetCritical_Z2"]={n="Recipe_FormulaFeetCritical_Z2",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_FormulaFeetFervor_Z2"]={n="Recipe_FormulaFeetFervor_Z2",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_FormulaFeetMagicPen_Z2"]={n="Recipe_FormulaFeetMagicPen_Z2",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_FormulaFeetMinorArmorPen"]={n="Recipe_FormulaFeetMinorArmorPen",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_FormulaFeetMinorCritical"]={n="Recipe_FormulaFeetMinorCritical",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_FormulaFeetMinorFervor"]={n="Recipe_FormulaFeetMinorFervor",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_FormulaFeetMinorMagicPen"]={n="Recipe_FormulaFeetMinorMagicPen",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_FormulaHandsDexterity_Z2"]={n="Recipe_FormulaHandsDexterity_Z2",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_FormulaHandsFaith_Z2"]={n="Recipe_FormulaHandsFaith_Z2",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_FormulaHandsIntelligence_Z2"]={n="Recipe_FormulaHandsIntelligence_Z2",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_FormulaHandsMinorDexterity"]={n="Recipe_FormulaHandsMinorDexterity",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_FormulaHandsMinorFaith"]={n="Recipe_FormulaHandsMinorFaith",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_FormulaHandsMinorIntelligence"]={n="Recipe_FormulaHandsMinorIntelligence",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_FormulaHandsMinorStrength"]={n="Recipe_FormulaHandsMinorStrength",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_FormulaHandsStrength_Z2"]={n="Recipe_FormulaHandsStrength_Z2",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_GildedCutBeryl"]={n="Recipe_GildedCutBeryl",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_GildedCutRuby"]={n="Recipe_GildedCutRuby",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_GracefulCopperPlate"]={n="Recipe_GracefulCopperPlate",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_GracefulEmbroidery"]={n="Recipe_GracefulEmbroidery",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_HonedCopperPlate"]={n="Recipe_HonedCopperPlate",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_HonedEmbroidery"]={n="Recipe_HonedEmbroidery",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_InvisibilityPotion"]={n="Recipe_InvisibilityPotion",r="Common",t="Recipe",sell=75},
  ["Recipe_LongBreathPotion"]={n="Recipe_LongBreathPotion",r="Common",t="Recipe",sell=50},
  ["Recipe_MinorShieldPotion"]={n="Recipe_MinorShieldPotion",r="Common",t="Recipe",sell=50},
  ["Recipe_MinorVelocityPotion"]={n="Recipe_MinorVelocityPotion",r="Common",t="Recipe",sell=50},
  ["Recipe_MysticCopperPlate"]={n="Recipe_MysticCopperPlate",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_MysticEmbroidery"]={n="Recipe_MysticEmbroidery",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_ResonantCutAgate"]={n="Recipe_ResonantCutAgate",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_ResonantCutAmber"]={n="Recipe_ResonantCutAmber",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_RunedCopperPlate"]={n="Recipe_RunedCopperPlate",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_RunedEmbroidery"]={n="Recipe_RunedEmbroidery",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_SanctifiedCopperPlate"]={n="Recipe_SanctifiedCopperPlate",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_SanctifiedEmbroidery"]={n="Recipe_SanctifiedEmbroidery",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_ShieldPotion_Z2"]={n="Recipe_ShieldPotion_Z2",r="Common",t="Recipe",sell=100},
  ["Recipe_SunderedCutAgate"]={n="Recipe_SunderedCutAgate",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_SunderedCutAmber"]={n="Recipe_SunderedCutAmber",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_SurgingCutAgate"]={n="Recipe_SurgingCutAgate",r="Uncommon",t="Recipe",sell=100},
  ["Recipe_SurgingCutAmber"]={n="Recipe_SurgingCutAmber",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_TemperedCutBeryl"]={n="Recipe_TemperedCutBeryl",r="Uncommon",t="Recipe",sell=50},
  ["Recipe_TemperedCutRuby"]={n="Recipe_TemperedCutRuby",r="Uncommon",t="Recipe",sell=100},
  ["RefillableFlask"]={n="Hess'tuss Flask",r="Common",t="HealthPotion",sell=1},
  ["ReinforcedCopperPlate"]={n="Reinforced Bronze Plate",r="Uncommon",t="AugmentBlacksmith",sell=1},
  ["ReinforcedEmbroidery"]={n="Reinforced Soft Embroidery",r="Uncommon",t="AugmentOutfitter",sell=1},
  ["Residues_Z1"]={n="Bright Residues",r="Uncommon",t="CraftingComponent",sell=2},
  ["ResonantCutAgate"]={n="Resonant Cut Agate",r="Uncommon",t="AugmentJeweller",sell=1},
  ["ResonantCutAmber"]={n="Resonant Cut Amber",r="Uncommon",t="AugmentJeweller",sell=1},
  ["Rock_Z1"]={n="Cracked Stone",r="Common",t="CraftingComponent",sell=3},
  ["RoyalJelly"]={n="Royal Jelly",r="Common",t="Food",sell=6},
  ["Ruby"]={n="Ruby",r="Uncommon",t="CraftingComponent",sell=20},
  ["RunedCopperPlate"]={n="Runed Bronze Plate",r="Uncommon",t="AugmentBlacksmith",sell=1},
  ["RunedEmbroidery"]={n="Runed Soft Embroidery",r="Uncommon",t="AugmentOutfitter",sell=1},
  ["SanctifiedCopperPlate"]={n="Sanctified Bronze Plate",r="Uncommon",t="AugmentBlacksmith",sell=1},
  ["SanctifiedEmbroidery"]={n="Sanctified Soft Embroidery",r="Uncommon",t="AugmentOutfitter",sell=1},
  ["Scepter_Flamie"]={n="Flame of Argol",r="Rare",t="Scepter",sell=1},
  ["Scepter_Start"]={n="Rehearsal Scepter",r="Uncommon",t="Scepter",sell=1},
  ["Scissors"]={n="Worn Scissors",r="Common",t="ToolOutfitter",sell=1},
  ["ScrollOfDexterity"]={n="Scroll Of Dexterity I",r="Common",t="Consumable",sell=1},
  ["ScrollOfFaith"]={n="Scroll Of Faith I",r="Common",t="Consumable",sell=1},
  ["ScrollOfIntelligence"]={n="Scroll Of Intelligence I",r="Common",t="Consumable",sell=1},
  ["ScrollOfStrength"]={n="Scroll Of Strength I",r="Common",t="Consumable",sell=1},
  ["ScrollOfVitality"]={n="Scroll Of Vitality I",r="Common",t="Consumable",sell=1},
  ["ShieldPotion_Z2"]={n="Shield Potion",r="Common",t="Potion",sell=1},
  ["Shield_Craft"]={n="Dominion",r="Rare",t="Shield",sell=1},
  ["Shield_Firebreath"]={n="Magma Mia",r="Rare",t="Shield",sell=1},
  ["Shield_OrbitWater"]={n="Crabgantua's Kneecap",r="Rare",t="Shield",sell=1},
  ["Shield_Start"]={n="Rough Shield",r="Uncommon",t="Shield",sell=1},
  ["ShopCurrency"]={n="ShopCurrency",r="Common",t="Currency",sell=1},
  ["Shoulders_RBee_Ass"]={n="Beewings",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RBee_FigCle"]={n="Queen Spikes",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RBee_Wiz"]={n="Hive Sprouts",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RCrimson_AssCle"]={n="Noble Mantle",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RCrimson_Cle"]={n="Judgment Spaulders",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RCrimson_FigWiz"]={n="Spaulders of Intangible Faith",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RKobold_Ass"]={n="Cheese-Covered Shoulderpads",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RKobold_Fig"]={n="Miner Ramparts",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RKobold_Wiz"]={n="Treasure Hunter's Straps",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RManfish_AssWiz"]={n="Fins of the First Fish",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RManfish_FigAss"]={n="Abyssal Shoulderplates",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_RManfish_WizCle"]={n="Delicate Marine Aiglets",r="Rare",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_Ass"]={n="Featherlight Straps of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_AssCle"]={n="Ethereal Straps of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_AssWiz"]={n="Infused Straps of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_Cle"]={n="Sacred Tippet of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_Fig"]={n="Reinforced Pauldrons of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_FigAss"]={n="Condensed Pauldrons of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_FigCle"]={n="Blessed Pauldrons of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_FigWiz"]={n="Protected Pauldrons of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_Wiz"]={n="Spellbound Tippet of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U1_WizCle"]={n="Runic Tippet of the Adventurer",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_Ass"]={n="Featherlight Straps of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_AssCle"]={n="Ethereal Straps of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_AssWiz"]={n="Infused Straps of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_Cle"]={n="Sacred Tippet of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_Fig"]={n="Reinforced Pauldrons of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_FigAss"]={n="Condensed Pauldrons of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_FigCle"]={n="Blessed Pauldrons of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_FigWiz"]={n="Protected Pauldrons of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_Wiz"]={n="Spellbound Tippet of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Shoulders_Z2U2_WizCle"]={n="Runic Tippet of the Nomad",r="Uncommon",t="Shoulders",sell=1},
  ["Sickle"]={n="Worn Sickle",r="Common",t="GearSickle",sell=50},
  ["SilverTray"]={n="Silver Tray",r="Common",t="CraftingComponent",sell=500},
  ["SimpleCauldron"]={n="Simple Cauldron",r="Common",t="CraftingComponent",sell=500},
  ["SkillPointBook_Red"]={n="Training Manual",r="Uncommon",t="SkillPointBook",sell=50},
  ["SkoverDriedHerbs"]={n="Mixed Herbs",r="Uncommon",t="CraftingComponent",sell=1},
  ["SkunkMeat_Z1"]={n="Skunk Meat",r="Common",t="Food",sell=2},
  ["SmallAlchemistCauldron"]={n="Minor Alchemist Cauldron",r="Rare",t="Potion",sell=1},
  ["SmallPouch"]={n="Small Pouch",r="Uncommon",t="Bag",sell=250},
  ["SoftFur"]={n="Soft Fur",r="Rare",t="CraftingComponent",sell=7},
  ["SparkHorse_01"]={n="Sparkling Horsean",r="Epic",t="Mount",sell=0},
  ["SparkSample"]={n="Spark Sample",r="Uncommon",t="CraftingComponent",sell=5},
  ["Spear_Eruption"]={n="Gorgon Ratsay’s Toothpick",r="Rare",t="Spear",sell=1},
  ["Spear_Goo"]={n="Lady Bee’s Ceremonial Stinger",r="Rare",t="Spear",sell=1},
  ["SpiritHeart_Z2"]={n="Ephemeral Heart",r="Common",t="CraftingComponent",sell=5},
  ["Staff_Craft"]={n="Radiance",r="Rare",t="Staff",sell=1},
  ["StaleBread"]={n="Stale Bread",r="Common",t="Food",sell=5},
  ["SteelIngot"]={n="Steel Ingot",r="Uncommon",t="CraftingComponent",sell=1},
  ["StoneOfCunning"]={n="Stone of Cunning",r="Rare",t="GearTrinket",sell=1},
  ["StoneOfPower"]={n="Stone of Power",r="Rare",t="GearTrinket",sell=1},
  ["StoneOfRighteousness"]={n="Stone of Rigtheousness",r="Rare",t="GearTrinket",sell=1},
  ["StoneOfWisdom"]={n="Stone of Wisdom",r="Rare",t="GearTrinket",sell=1},
  ["Stone_Ore_Z1"]={n="Glittering Stone",r="Rare",t="CraftingComponent",sell=15},
  ["StrangeSpores"]={n="Strange Spores",r="Rare",t="CraftingComponent",sell=15},
  ["StrongCutAgate"]={n="Strong Cut Agate",r="Uncommon",t="AugmentJeweller",sell=1},
  ["StrongCutAmber"]={n="Strong Cut Amber",r="Uncommon",t="AugmentJeweller",sell=1},
  ["SunderedCutAgate"]={n="Sundered Cut Agate",r="Uncommon",t="AugmentJeweller",sell=1},
  ["SunderedCutAmber"]={n="Sundered Cut Amber",r="Uncommon",t="AugmentJeweller",sell=1},
  ["SurgingCutAgate"]={n="Surging Cut Agate",r="Uncommon",t="AugmentJeweller",sell=1},
  ["SurgingCutAmber"]={n="Surging Cut Amber",r="Uncommon",t="AugmentJeweller",sell=1},
  ["SweetRoot"]={n="Sweet Root",r="Common",t="Food",sell=5},
  ["Sword_Craft"]={n="Glory",r="Rare",t="Sword",sell=1},
  ["Sword_Start"]={n="Light Practice Sword",r="Uncommon",t="Sword",sell=1},
  ["Sword_Swarm"]={n="Beefury, Blessed Blade of the Farseeker",r="Rare",t="Sword",sell=1},
  ["TPDeathStone"]={n="Death Stone",r="Uncommon",t="Consumable",sell=15},
  ["TannedLeather"]={n="Tanned Light Leather",r="Uncommon",t="CraftingComponent",sell=1},
  ["TeleportationStone"]={n="Sparkstone",r="Common",t="Usable",sell=1},
  ["TemperedCutBeryl"]={n="Tempered Cut Beryl",r="Uncommon",t="AugmentJeweller",sell=1},
  ["TemperedCutRuby"]={n="Tempered Cut Ruby",r="Uncommon",t="AugmentJeweller",sell=1},
  ["Thrown_Seeds"]={n="Ipheion, Star Blossom",r="Rare",t="Thrown",sell=1},
  ["TinOre"]={n="Tin Ore",r="Common",t="Ore",sell=5},
  ["TinProspecting"]={n="Tin Prospecting",r="Common",t="Prospecting",sell=0},
  ["Trinket_Bee"]={n="Eternal Flower Heart",r="Rare",t="GearTrinket",sell=1},
  ["Trinket_Crimson"]={n="Lost Relic Found",r="Rare",t="GearTrinket",sell=1},
  ["Trinket_Kobold"]={n="Raclette Pan",r="Rare",t="GearTrinket",sell=1},
  ["Trinket_Manfish"]={n="Poetrident",r="Rare",t="GearTrinket",sell=1},
  ["TungsteneIngot"]={n="Tungstene Ingot",r="Rare",t="CraftingComponent",sell=1},
  ["TungsteneOre"]={n="Tungstene Ore",r="Rare",t="Ore",sell=7},
  ["TungsteneProspecting"]={n="Tungstene Prospecting",r="Common",t="Prospecting",sell=0},
  ["UpgradeAll"]={n="Spark Dust",r="Uncommon",t="Misc",sell=2},
  ["UpgradeEpic"]={n="Spark Crystal",r="Epic",t="Misc",sell=15},
  ["UpgradeRare"]={n="Spark Shard",r="Rare",t="Misc",sell=7},
  ["Vial"]={n="Vial",r="Common",t="CraftingComponent",sell=30},
  ["Waist_RBee_AssCle"]={n="Propolis Heart of Apix",r="Rare",t="Waist",sell=1},
  ["Waist_RBee_FigWiz"]={n="Aura of the Honeycomb",r="Rare",t="Waist",sell=1},
  ["Waist_RBee_Wiz"]={n="Dancing Hivetree Belt",r="Rare",t="Waist",sell=1},
  ["Waist_RCrimson_AssWiz"]={n="Relic of Cernoros the Lost King",r="Rare",t="Waist",sell=1},
  ["Waist_RCrimson_FigAss"]={n="Forbidden Insignia of Silence",r="Rare",t="Waist",sell=1},
  ["Waist_RCrimson_WizCle"]={n="Curse of the Immortal",r="Rare",t="Waist",sell=1},
  ["Waist_RKobold_Ass"]={n="Relic of the Four Hundred",r="Rare",t="Waist",sell=1},
  ["Waist_RKobold_AssCle_Craft"]={n="Belt of the Great Fermentation",r="Rare",t="Waist",sell=1},
  ["Waist_RKobold_Cle"]={n="Knotr'Edam",r="Rare",t="Waist",sell=1},
  ["Waist_RKobold_FigCle"]={n="Unity of the Thirty Kingdoms",r="Rare",t="Waist",sell=1},
  ["Waist_RManfish_AssWiz"]={n="Emblem of the Third Wave",r="Rare",t="Waist",sell=1},
  ["Waist_RManfish_Fig"]={n="Caryapsid's Coccyx",r="Rare",t="Waist",sell=1},
  ["Waist_RManfish_WizCle"]={n="Ceremonial Nepsid Belt",r="Rare",t="Waist",sell=1},
  ["Waist_RManfish_Wiz_Craft"]={n="Barnacle Waistwrap",r="Rare",t="Waist",sell=1},
  ["Waist_Z1U1_Ass"]={n="Featherlight Belt of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U1_AssCle"]={n="Ethereal Belt of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U1_AssWiz"]={n="Infused Belt of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U1_Cle"]={n="Sacred Belt of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U1_Fig"]={n="Reinforced Pteruge of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U1_FigAss"]={n="Condensed Pteruge of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U1_FigCle"]={n="Blessed Pteruge of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U1_FigWiz"]={n="Protected Pteruge of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U1_Wiz"]={n="Spellbound Belt of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U1_WizCle"]={n="Runic Belt of the Exile",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_Ass"]={n="Featherlight Belt of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_AssCle"]={n="Ethereal Belt of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_AssWiz"]={n="Infused Belt of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_Cle"]={n="Sacred Belt of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_Fig"]={n="Reinforced Pteruge of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_FigAss"]={n="Condensed Pteruge of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_FigCle"]={n="Blessed Pteruge of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_FigWiz"]={n="Protected Pteruge of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_Wiz"]={n="Spellbound Belt of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z1U2_WizCle"]={n="Runic Belt of the Trespasser",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_Ass"]={n="Featherlight Belt of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_AssCle"]={n="Ethereal Belt of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_AssWiz"]={n="Infused Belt of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_Cle"]={n="Sacred Belt of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_Fig"]={n="Reinforced Pteruge of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_FigAss"]={n="Condensed Pteruge of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_FigCle"]={n="Blessed Pteruge of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_FigWiz"]={n="Protected Pteruge of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_Wiz"]={n="Spellbound Belt of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U1_WizCle"]={n="Runic Belt of the Adventurer",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U2_Ass"]={n="Featherlight Belt of the Nomad",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U2_AssCle"]={n="Ethereal Belt of the Nomad",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U2_AssWiz"]={n="Infused Belt of the Nomad",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U2_Cle"]={n="Sacred Belt of the Nomad",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U2_Fig"]={n="Reinforced Pteruge of the Nomad",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U2_FigAss"]={n="Condensed Pteruge of the Nomad",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U2_FigCle"]={n="Waist_Z2U2_FigCle",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U2_Wiz"]={n="Spellbound Belt of the Nomad",r="Uncommon",t="Waist",sell=1},
  ["Waist_Z2U2_WizCle"]={n="Runic Belt of the Nomad",r="Uncommon",t="Waist",sell=1},
  ["Wand"]={n="Wooden Wand",r="Common",t="ToolEnchanter",sell=1},
  ["Weightstone"]={n="Weightstone",r="Common",t="Consumable",sell=1},
  ["Wheat"]={n="Wheat",r="Common",t="Food",sell=4},
  ["Whetstone"]={n="Whetstone",r="Common",t="Consumable",sell=1},
  ["Wing_Z1"]={n="Diaphanous Wings",r="Common",t="CraftingComponent",sell=2},
  ["WolfMeat_Z1"]={n="Wolf Meat",r="Common",t="Food",sell=2},
  ["WorldLoot"]={n="WorldLoot",r="Common",t="Misc",sell=1},
  ["WorldLootWithAffinity"]={n="WorldLootWithAffinity",r="Common",t="Misc",sell=1},
  ["WorldRecipeWithJob"]={n="WorldRecipeWithJob",r="Common",t="Misc",sell=1},
  ["Z1_WeaponBundle"]={n="Mysterious Cache",r="Rare",t="LootableContainer",sell=1},
  ["ZealotusPetal"]={n="Zealotus Petal",r="Rare",t="CraftingComponent",sell=10},
  ["FiendishEye"]={n="Fiendish Eye",r="Rare",t="CraftingComponent",sell=25},
  ["FragmentOfChaos"]={n="Fragment of Nuon",r="Uncommon",t="CraftingComponent",sell=25},
  ["MoteOfChaos"]={n="Mote of Nuon",r="Common",t="CraftingComponent",sell=1},
  ["VeiledWing"]={n="Veiled Wing",r="Uncommon",t="CraftingComponent",sell=5},
  ["DemonicHorn"]={n="Demonic Horn",r="Common",t="CraftingComponent",sell=2},
  ["TailSlice"]={n="Tail Slice",r="Common",t="CraftingComponent",sell=2},
  ["Soulstone_Z1_1"]={n="Baphometal Soulstone",r="Rare",t="Soulstone",sell=1},
  ["Soulstone_Z1_2"]={n="Luciferrari Soulstone",r="Rare",t="Soulstone",sell=1},
  ["Soulstone_Z1_3"]={n="Belzebeat Soulstone",r="Rare",t="Soulstone",sell=1},
  ["Soulstone_Z1_4"]={n="Ariana Grandemon Soulstone",r="Epic",t="Soulstone",sell=1},
  ["Soulstone_Z2_1"]={n="Lilithium  Soulstone",r="Rare",t="Soulstone",sell=1},
  ["Soulstone_Z2_2"]={n="Asmodeaf Soulstone",r="Rare",t="Soulstone",sell=1},
  ["Soulstone_Z2_3"]={n="Mortalkombaal Soulstone",r="Rare",t="Soulstone",sell=1},
  ["Soulstone_Z2_4"]={n="Kristian Belial Soulstone",r="Epic",t="Soulstone",sell=1},
}
local PLUGIN_VERSION = "1.0.1"

local CURRENCIES = {
    { kind = "Gold",         label = "Gold" },
    { kind = "CraftPoint",   label = "Craft point" },
    { kind = "DemonicSoul", label = "Demonic soul" },
}

local DEFAULT_ITEMS = {
    "Ephemeral Heart",
    "Kobold Swiss",
}

local enabled = true
local track_currency = true
local show_editor = false
local tracked_items = {}   -- { name, kind? } kind resolved from DB_items when absent
local new_item_name = ""
local inventory_available = false
local last_refresh = 0.0
local REFRESH_SEC = 0.5
local counts_by_kind = {}   -- lowercased kind -> total quantity
local currency_amounts = {} -- currency kind -> amount
local items_by_name = {}    -- lowercased display name -> item id
local currency_visible = {}   -- kind -> bool
local debug_currencies = false
local currency_api_lines = {} -- last raw snapshot for the debug panel

local function default_currency_visible()
    local out = {}
    for _, cur in ipairs(CURRENCIES) do
        out[cur.kind] = true
    end
    return out
end

local function escape_field(s)
    if not s or s == "" then return "" end
    return (tostring(s):gsub("\\", "\\\\"):gsub("|", "\\|"):gsub("\n", " "))
end

local function unescape_field(s)
    if not s or s == "" then return "" end
    return (s:gsub("\\|", "|"):gsub("\\\\", "\\"))
end

local function norm_key(s)
    return (tostring(s or ""):lower():gsub("^%s+", ""):gsub("%s+$", ""))
end

local function rebuild_name_index()
    items_by_name = {}
    for item_id, item in pairs(DB_items) do
        if type(item) == "table" and item.n and item.n ~= "" then
            items_by_name[norm_key(item.n)] = item_id
        end
    end
end

local function resolve_kind(name)
    return items_by_name[norm_key(name)]
end

local function resolve_entry_kind(entry)
    if entry.kind and entry.kind ~= "" then
        return entry.kind
    end
    return resolve_kind(entry.name)
end

local function hydrate_entry_kinds()
    for _, entry in ipairs(tracked_items) do
        if not entry.kind or entry.kind == "" then
            entry.kind = resolve_kind(entry.name)
        end
    end
end

local function serialize_items()
    local lines = { "v3" }
    for _, entry in ipairs(tracked_items) do
        table.insert(lines, escape_field(entry.name))
    end
    return table.concat(lines, "\n")
end

local function deserialize_items(blob)
    tracked_items = {}
    if not blob or blob == "" then return end

    local version = "v1"
    local first = true
    for line in blob:gmatch("[^\n]+") do
        if first then
            first = false
            if line == "v2" or line == "v3" then
                version = line
            else
                tracked_items = { { name = unescape_field(line), kind = nil } }
            end
        elseif version == "v2" then
            local name, kind = line:match("^(.-)|([^|]+)$")
            if not name then
                name = line
                kind = nil
            end
            name = unescape_field(name)
            kind = kind and unescape_field(kind) or nil
            if name ~= "" then
                table.insert(tracked_items, { name = name, kind = kind })
            end
        else
            local name = unescape_field(line)
            if name ~= "" then
                table.insert(tracked_items, { name = name, kind = nil })
            end
        end
    end
end

local function format_currency_entry(index, entry)
    if type(entry) ~= "table" then
        return string.format("[%s] %s", tostring(index), tostring(entry))
    end

    local parts = {}
    for key, value in pairs(entry) do
        parts[#parts + 1] = string.format("%s=%s", tostring(key), tostring(value))
    end
    table.sort(parts)
    if #parts == 0 then
        return string.format("[%s] (empty table)", tostring(index))
    end
    return string.format("[%s] %s", tostring(index), table.concat(parts, ", "))
end

local function snapshot_currencies_api(ok, currencies)
    currency_api_lines = {}

    if type(farever.player.currencies) ~= "function" then
        table.insert(currency_api_lines,
            "farever.player.currencies() missing (needs farever-mod v1.2.3+)")
        return
    end
    if not ok then
        table.insert(currency_api_lines, "pcall(farever.player.currencies) failed")
        return
    end
    if type(currencies) ~= "table" then
        table.insert(currency_api_lines,
            string.format("returned %s: %s", type(currencies), tostring(currencies)))
        return
    end

    table.insert(currency_api_lines, string.format("#currencies = %d", #currencies))

    local array_count = 0
    for index, entry in ipairs(currencies) do
        array_count = array_count + 1
        table.insert(currency_api_lines, format_currency_entry(index, entry))
    end

    if array_count == 0 then
        for key, value in pairs(currencies) do
            if type(value) == "table" then
                table.insert(currency_api_lines, format_currency_entry(key, value))
            else
                table.insert(currency_api_lines,
                    string.format("[%s] %s", tostring(key), tostring(value)))
            end
        end
    end

    if #currency_api_lines == 1 then
        table.insert(currency_api_lines, "(empty)")
    end
end

local function log_currency_snapshot()
    farever.log.info("item_tracker: farever.player.currencies() snapshot")
    for _, line in ipairs(currency_api_lines) do
        farever.log.info("  " .. line)
    end
end

local function copy_default_items()
    tracked_items = {}
    for _, name in ipairs(DEFAULT_ITEMS) do
        table.insert(tracked_items, { name = name, kind = nil })
    end
end

local function refresh_counts()
    counts_by_kind = {}
    currency_amounts = {}
    inventory_available = false

    if type(farever.player.inventory) == "function" then
        local ok, items = pcall(farever.player.inventory)
        if ok and type(items) == "table" then
            inventory_available = true
            for _, entry in ipairs(items) do
                if type(entry) == "table" and entry.kind and entry.kind ~= "" then
                    local key = norm_key(entry.kind)
                    local qty = entry.stack
                    if type(qty) ~= "number" or qty < 1 then
                        qty = 1
                    end
                    counts_by_kind[key] = (counts_by_kind[key] or 0) + qty
                end
            end
        end
    end

    if type(farever.player.currencies) == "function" then
        local ok, currencies = pcall(farever.player.currencies)
        snapshot_currencies_api(ok, currencies)
        if ok and type(currencies) == "table" then
            for _, entry in ipairs(currencies) do
                if type(entry) == "table" and entry.kind and entry.kind ~= "" then
                    currency_amounts[entry.kind] = entry.amount or 0
                end
            end
        end
    else
        snapshot_currencies_api(false, nil)
    end
end

local function currency_label(kind)
    for _, cur in ipairs(CURRENCIES) do
        if cur.kind == kind then
            return cur.label
        end
    end
    local item = DB_items[kind]
    if item and item.n and item.n ~= "" then
        return item.n
    end
    return kind
end

local function render_currency_counts()
    if show_editor then
        for _, cur in ipairs(CURRENCIES) do
            local visible = currency_visible[cur.kind] == true
            local new_visible, changed = imgui.checkbox("##cur_" .. cur.kind, visible)
            if changed then
                currency_visible[cur.kind] = new_visible
                farever.store.set("currency_" .. cur.kind, new_visible)
            end
            imgui.same_line()

            local amount = currency_amounts[cur.kind]
            local label = currency_label(cur.kind)
            if amount == nil then
                imgui.text(string.format("%s: —", label))
            else
                imgui.text(string.format("%s: %d", label, amount))
            end
        end
        return #CURRENCIES > 0
    end

    local any = false
    for _, cur in ipairs(CURRENCIES) do
        if currency_visible[cur.kind] then
            any = true
            local amount = currency_amounts[cur.kind]
            local label = currency_label(cur.kind)
            if amount == nil then
                imgui.text(string.format("%s: —", label))
            else
                imgui.text(string.format("%s: %d", label, amount))
            end
        end
    end
    return any
end

local function count_for_entry(entry)
    local kind = resolve_entry_kind(entry)
    if not kind or kind == "" then
        return nil
    end
    return counts_by_kind[norm_key(kind)] or 0
end

local function name_suggestions(prefix, limit)
    local key = norm_key(prefix)
    if key == "" then
        return {}
    end

    local matches = {}
    for lname, item_id in pairs(items_by_name) do
        if lname:sub(1, #key) == key then
            local item = DB_items[item_id]
            matches[#matches + 1] = item and item.n or lname
        end
    end
    table.sort(matches)
    if limit and #matches > limit then
        local trimmed = {}
        for i = 1, limit do
            trimmed[i] = matches[i]
        end
        return trimmed
    end
    return matches
end

local function db_item_count()
    local n = 0
    for _ in pairs(DB_items) do n = n + 1 end
    return n
end

local function render_tracked_counts()
    if #tracked_items == 0 then
        imgui.text("(none)")
        return
    end

    if not inventory_available then
        imgui.text("(needs farever-mod v1.2.1+)")
    end

    for i, entry in ipairs(tracked_items) do
        if show_editor then
            if imgui.button("x##rm_" .. i) then
                table.remove(tracked_items, i)
                farever.store.set("tracked_items", serialize_items())
                refresh_counts()
                break
            end
            imgui.same_line()
        end

        local count = count_for_entry(entry)
        if count == nil then
            imgui.text(string.format("%s: (unknown name)", entry.name))
        elseif not inventory_available then
            imgui.text(string.format("%s: —", entry.name))
        else
            imgui.text(string.format("%s: %d", entry.name, count))
        end
    end
end

local function render_editor()
    imgui.text("Item name")
    local new_name, name_changed = imgui.input_text("##new_item", new_item_name)
    if name_changed then
        new_item_name = new_name
    end

    imgui.same_line()
    if imgui.button("Add") then
        local trimmed = new_item_name:gsub("^%s+", ""):gsub("%s+$", "")
        if trimmed ~= "" then
            local exists = false
            for _, entry in ipairs(tracked_items) do
                if norm_key(entry.name) == norm_key(trimmed) then
                    exists = true
                    break
                end
            end
            if not exists then
                table.insert(tracked_items, {
                    name = trimmed,
                    kind = resolve_kind(trimmed),
                })
                farever.store.set("tracked_items", serialize_items())
                refresh_counts()
            end
        end
        new_item_name = ""
    end

    local suggestions = name_suggestions(new_item_name, 5)
    if #suggestions > 0 then
        for _, suggestion in ipairs(suggestions) do
            imgui.text_colored(0.55, 0.55, 0.55, 1.0, "  " .. suggestion)
        end
    end
end

local function render_currency_debug()
    local new_debug, changed = imgui.checkbox("Debug API currencies", debug_currencies)
    if changed then
        debug_currencies = new_debug
        farever.store.set("debug_currencies", debug_currencies)
        if debug_currencies then
            log_currency_snapshot()
        end
    end

    if not debug_currencies then
        return
    end

    imgui.text("farever.player.currencies():")
    for _, line in ipairs(currency_api_lines) do
        imgui.text("  " .. line)
    end

    if imgui.button("Log snapshot") then
        log_currency_snapshot()
    end
end

function on_init()
    enabled = farever.store.get("enabled", true)
    track_currency = farever.store.get("track_currency", true)
    show_editor = farever.store.get("show_editor", false)
    debug_currencies = farever.store.get("debug_currencies", false)
    currency_visible = default_currency_visible()
    for _, cur in ipairs(CURRENCIES) do
        currency_visible[cur.kind] = farever.store.get(
            "currency_" .. cur.kind, currency_visible[cur.kind])
    end
    local blob = farever.store.get("tracked_items", "")
    if blob == "" then
        copy_default_items()
        farever.store.set("tracked_items", serialize_items())
    else
        deserialize_items(blob)
        if #tracked_items == 0 then
            copy_default_items()
            farever.store.set("tracked_items", serialize_items())
        end
    end

    rebuild_name_index()
    hydrate_entry_kinds()

    new_item_name = ""
    inventory_available = false
    last_refresh = 0.0
    counts_by_kind = {}
    currency_amounts = {}
    currency_api_lines = {}
    farever.log.info(string.format(
        "loaded: item_tracker v%s (%d tracked, %d known items)",
        PLUGIN_VERSION, #tracked_items, db_item_count()))
end

function on_render()
    local now = farever.now()
    if now - last_refresh >= REFRESH_SEC then
        refresh_counts()
        last_refresh = now
    end

    imgui.separator()

    if show_editor then
        local new_track, changed = imgui.checkbox("Track currency", track_currency)
        if changed then
            track_currency = new_track
            farever.store.set("track_currency", track_currency)
        end
    end

    if track_currency then
        render_currency_counts()
    end

    if show_editor then
        imgui.separator()
        local new_enabled, changed = imgui.checkbox("Track items", enabled)
        if changed then
            enabled = new_enabled
            farever.store.set("enabled", enabled)
        end
    end

    if enabled then
        if track_currency or show_editor then
            imgui.separator()
        end
        render_tracked_counts()
    end

    imgui.separator()

    if imgui.button(show_editor and "Done" or "Edit") then
        show_editor = not show_editor
        farever.store.set("show_editor", show_editor)
    end

    if show_editor and enabled then
        render_editor()
    end
end

function on_event(name, data)
    if name == "hero_locked" then
        hydrate_entry_kinds()
        refresh_counts()
    end
end
