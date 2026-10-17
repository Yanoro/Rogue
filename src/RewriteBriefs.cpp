#include "RewriteBriefs.hpp"

#include "TileArt.hpp"

namespace RewriteBriefs {

PromptRewriter::Brief Tile() {
  // What the rewriter may promise about a tile, and no more. Deliberately not the
  // whole TILEDESIGNER.md system prompt: that is a reply contract for producing JSON,
  // and the user is describing a picture. What matters here is what the tile
  // generator can actually deliver, so a rewrite does not enrich toward detail that
  // will come back wrong.
  PromptRewriter::Brief brief;
  brief.kind = "tile";

  std::string context =
      "The text describes one 32x32 ground tile for a top-down 2D game. The tile is "
      "drawn as a single sprite, seen from directly above, filling its square cell "
      "edge to edge. It can show a surface and the detail on it -- boards, stone "
      "joints, gravel, moss, wear, colour, texture -- and it can be walkable or "
      "blocking. It cannot show a picture of an object standing on the ground, cannot "
      "be animated, cannot hold things, cannot produce resources and cannot have any "
      "effect on the game beyond whether it blocks movement. It is a surface, not a "
      "scene: there is no horizon, no side view and no shadow cast outside the cell.\n";

  // The pack's own prose, which is the style the rewrite has to belong to. Read from
  // the pack rather than restated here, so a second art pack changes what the
  // rewriter aligns with and no code changes.
  const TileArt::PackStyle style = TileArt::CurrentPackStyle();
  if (!style.description.empty()) {
    context += "\nThe art this sits among is described as: " + style.description +
               "\nMatch that style. Prefer materials, colours and wear that already "
               "exist in it over inventing new ones.\n";
  } else {
    // The pack has no description. Said in the prompt rather than left out, because
    // the rewriter should know it is aligning with nothing in particular instead of
    // filling the gap with its own idea of the genre.
    context +=
        "\nThe art pack has no written description, so align with the tile's own name "
        "and colours and keep the description grounded in ordinary materials rather "
        "than a specific art style.\n";
  }

  context +=
      "\nKeep the rewrite to what a single sprite can show, and keep it about the "
      "surface rather than about what happens on it.";

  brief.context = std::move(context);
  return brief;
}

PromptRewriter::Brief Object() {
  // What the object designer can deliver, and what it cannot. Same structure as Tile()
  // and deliberately a separate brief -- see the header for why the two must not be one.
  PromptRewriter::Brief brief;
  brief.kind = "object";

  std::string context =
      "The text describes one object that stands in a top-down 2D world: a thing an agent "
      "can see, walk around, pick up, open, harvest or use -- a crate, a barrel, a "
      "candelabra, a workbench, an altar, a young tree. It is drawn as a small block of "
      "ASCII art (a few rows of a few characters) in a limited palette, and it also has a "
      "single-character fallback.\n"
      "\nWhat makes something an object is that it *does* something: it can be interacted "
      "with, carried, block movement, hold a number of items, be harvested for a yield on "
      "a timer, be crafted at from a set of recipes, grow into another object over time, "
      "be placed down in the world, require equipment to use, or teach a skill when used. "
      "Those are the only capabilities that exist, and an object needs at least one of "
      "them -- a thing that only sits there looking like something is ground, and ground "
      "is a tile, not an object.\n"
      "\nDescribe the object itself: what it is made of, its size and shape, how worn or "
      "new it is, and what it is for. Say what it does in plain words (\"holds a few "
      "things\", \"you can saw at it\", \"it is growing into a tree\") rather than naming "
      "an engine capability. Do not ask for behaviour beyond the list above -- no "
      "containers that sort themselves, no objects that move on their own, nothing "
      "animated, nothing that changes the world except by being used.";

  const TileArt::PackStyle style = TileArt::CurrentPackStyle();
  if (!style.description.empty()) {
    context += "\n\nThe art this sits among is described as: " + style.description +
               "\nMatch that style. Prefer materials, colours and wear that already "
               "exist in it over inventing new ones.";
  } else {
    context +=
        "\n\nThe art pack has no written description, so align with the object's own "
        "name and colours and keep it in the ordinary village register rather than a "
        "specific art style.";
  }

  brief.context = std::move(context);
  return brief;
}

PromptRewriter::Brief Blueprint() {
  // The building equivalent: what a design can express, so the rewrite does not ask
  // for something the design language has no way to build.
  //
  // The materials are not listed here on purpose. A list of materials is exactly what
  // must not go into a rewrite prompt: the model copies lists rather than reading them
  // as constraints, which is the bug that made the first generated sheet a palette
  // swatch sheet (see tools/gen_tile_art.py). It describes the kind of building
  // instead, and the system prompt the design is sent with carries the real
  // vocabulary.
  PromptRewriter::Brief brief;
  brief.kind = "building";

  brief.context =
      "The text describes a building the game will construct on a map, written as a "
      "design: a footprint of cells, walls, floors, a doorway and whatever furniture "
      "the building needs. A design covers a modest area -- a house or a workshop, not "
      "a district -- and is single-storey. Walls enclose the interior and at least one "
      "doorway makes it reachable; anything inside sits on a floor rather than in a "
      "wall. Furniture can be placed and can block movement, and it cannot hold, "
      "produce or craft anything merely by being described.\n"
      "\nDescribe the building's shape and layout, its materials, its size in rough "
      "terms, where the entrance faces and what stands inside it. Keep it to what can "
      "be drawn as a floor plan of a few dozen cells, and prefer materials and "
      "furniture in the ordinary village register over exotic ones.";

  return brief;
}

}  // namespace RewriteBriefs
