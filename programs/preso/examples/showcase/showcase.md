---
theme: paper
paginate: true
footer: Preso · scenes
transition: dissolve
---

<!-- _class: title -->
# Preso's scenes

## Every part of its scripting, a slide each

<!--
Each slide's picture is a script in this folder: an AngelScript file that draws in its region
and moves a step at a time. Every one also runs alone: `angel shapes.as` plays it in a window.
-->

---

# Shapes

![script w:1100 h:440](shapes.as)

<!-- Rectangles (rounded), circles, ellipses, regular polygons, stars, sectors; arcs, curves,
lines and arrows with heads of eight kinds at either end, bent or dashed; marks. -->

---

# Animations

![script w:1100 h:440](animations.as)

<!-- grow, move with an ease, rotateBy and fillTo together, indicate, wiggle, transform; then a
lagged play: each dot a little after the one before. -->

---

# Arrays

![script w:1100 h:420](arrays.as)

<!-- SceneArray: highlight, swap (along arcs), pointTo for named pointers. A pass a step. -->

---

# Tables

![script w:1100 h:360](tables.as)

<!-- SceneTable: set, a header row, highlight cells, rows and columns. -->

---

# Graphs

![script w:1100 h:440](graphs.as)

<!-- SceneGraph: nodes and edges, layoutTree, visit, highlightPath played with a lag, then the
nodes moved round a circle: the edges follow them. -->

---

# Code

![script w:1100 h:440](code.as)

<!-- SceneCode: highlight lines, the rest dimmed; transformTo slides the lines it keeps. -->

---

# Notes on a picture

![script w:1100 h:440](notes.as)

<!-- SceneBrace, SceneCallout, and SceneLabel, which stays beside what it labels. -->

---

# Things that follow

![script w:1100 h:440](updaters.as)

<!-- follow(): a function called for an object before each frame. moveAlong(): along any path,
turned to face its way if asked. -->

---

# Charts

![script w:1100 h:440](charts.as)

<!-- Bar, pie and line charts from values and labels: build() and setValue(). -->

---

# Plots

![script w:1100 h:440](plots.as)

<!-- SceneAxes with ticks and numbers, plot() of a function, a dot along the curve. -->

---

# The camera

![script w:1100 h:440](camera.as)

<!-- zoomTo an object, panTo and zoomBy together, resetCamera. -->

---

<!-- interactive: true -->
# Something to play with

![script w:1100 h:440](interactive.as)

<!-- Widgets in the scene: the slider and the toggle redraw the plot. Escape leaves. -->

---

# And to end

![script w:1100 h:440](finale.as)

<!-- Icons of the system's theme, marks written, and confetti. -->
