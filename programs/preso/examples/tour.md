---
theme: paper
paginate: true
footer: Preso · a tour
transition: wipe
---

<!-- _class: title -->
# Preso

## Presentations written in Markdown

<!--
Speaker notes are any comment that isn't a directive. Started over SSH, they show in your
terminal: `ssh root@bmds0 preso tour.md`. Next with the right arrow, Space, PageDown or `n`.
-->

---

# A deck is one Markdown file

* Slides are separated by `---` lines
* The front matter sets the theme, page numbers, a footer
* `<!-- name: value -->` sets a directive from a slide on
* `<!-- _name: value -->` sets it for one slide

Marp's format: the same file shows in Marp on a PC.

<!-- `*` lists appear an item a step; `-` lists all at once. -->

---

# Text

Paragraphs with **bold**, *italic*, `code` and ~~strikethrough~~.

> A block quote, for something someone said.

1. Numbered lists
2. with nesting
   - like this
   - and this

---

# Code, coloured by its language

```c
#include <stdio.h>

int main(void)
{
        printf("Hello from the SD card\n");
        return 0;
}
```

---

# Tables

| Key | Does |
|---|---|
| → Space PageDown | the next step |
| ← PageUp | the step before |
| Tab | the overview of every slide |
| B / W | a black or white screen |
| 12 Enter | slide 12 |

---

# A script draws here

![script w:900 h:360](tour/orbit.as)

<!-- Scripts are AngelScript, sandboxed to the deck's folder. `preso --api-help` lists what they have. -->

---

# Steps in a script

![script w:900 h:380](tour/build.as)

<!-- Each pause() in the script is a step: Next plays the next part, Previous goes back. -->

---

<!-- interactive: true -->
# Something to play with

![script w:900 h:380](tour/slider.as)

<!-- An interactive region: Tab enters it (or a click), Escape leaves. -->

---

# Code that runs

```angel run
for (int n = 1; n <= 5; n++)
        println(n * n);
```

<!-- An `angel run` block shows its code, and runs it on the next step into a pane below. -->

---

<!-- transition: dissolve -->
# Transitions

Between slides: `cut`, `wipe`, `dissolve`, `pixelate`, `crt`, `type`.

This one came in with `dissolve`.

---

<!-- transition: type -->
# Typed

This slide types itself, as a teletype would.

---

# Running it

* `preso tour.md`, or from slide 4: `preso tour.md 4`
* `preso check tour.md` reports what is wrong
* `preso export tour.md pictures` draws each step to a PNG
* `preso --loop tour.md` goes round, for a stand at a show
* `<!-- auto: 10s -->` steps on by itself
* Save the file and the deck reloads, at the same slide

---

<!-- _class: lead -->
# Now write your own

`preso theme vga` prints a theme to start yours from.
