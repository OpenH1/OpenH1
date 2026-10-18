/* The page's pictures and clips, from the media list at the end of
   index.html (<script id="media-data">). A section with nothing in it stays
   hidden. How to add media: website/README.md, "Pictures and clips". */
(function () {
  "use strict";

  var source = document.getElementById("media-data");
  if (!source) return;
  var media;
  try { media = JSON.parse(source.textContent); } catch (e) {
    if (window.console) console.warn("media list: not valid JSON, so no media is shown:", e.message);
    return;
  }

  var compares = media.compare || [];
  var videos = media.video || [];
  var gallery = media.gallery || [];
  var reduceMotion = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)");
  var SVG = "http://www.w3.org/2000/svg";

  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text) node.textContent = text;
    return node;
  }

  function icon(path) {
    var svg = document.createElementNS(SVG, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("aria-hidden", "true");
    var p = document.createElementNS(SVG, "path");
    p.setAttribute("fill", "currentColor");
    p.setAttribute("d", path);
    svg.appendChild(p);
    return svg;
  }

  function image(src, alt, width, height, eager) {
    var img = el("img");
    img.src = src;
    img.alt = alt || "";
    if (width) img.width = width;
    if (height) img.height = height;
    img.decoding = "async";
    img.loading = eager ? "eager" : "lazy";
    img.draggable = false;
    return img;
  }

  function frame(item) {
    var box = el("div", "frame");
    box.style.aspectRatio = (item.width || 16) + " / " + (item.height || 9);
    return box;
  }

  function caption(figure, text) {
    if (text) figure.appendChild(el("figcaption", "", text));
  }

  /* A before/after pair: the "before" picture is clipped to the left of
     the line. The frame takes the pointer (drag or tap, mouse or touch);
     a range input, invisible but focusable, takes the keyboard and screen
     readers. */
  function compare(item, eager) {
    var before = item.beforeLabel || "Before";
    var after = item.afterLabel || "After";
    var figure = el("figure", "compare");
    var box = frame(item);
    box.style.setProperty("--pos", "50%");

    box.appendChild(image(item.after, item.afterAlt, item.width, item.height, eager));
    var clip = el("div", "compare-before");
    clip.appendChild(image(item.before, item.beforeAlt, item.width, item.height, eager));
    box.appendChild(clip);
    box.appendChild(el("span", "compare-tag before", before));
    box.appendChild(el("span", "compare-tag after", after));
    box.appendChild(el("span", "compare-line"));
    var knob = el("span", "compare-knob");
    knob.appendChild(icon("M9 6 3 12l6 6V6zm6 0v12l6-6-6-6z"));
    box.appendChild(knob);

    var range = el("input", "compare-range");
    range.type = "range";
    range.min = "0";
    range.max = "100";
    range.step = "1";
    range.value = "50";
    range.setAttribute("aria-label", "Compare " + before + " (left) with " + after + " (right)");
    box.appendChild(range);

    function set(value) {
      value = Math.max(0, Math.min(100, value));
      box.style.setProperty("--pos", value + "%");
      range.value = String(Math.round(value));
      range.setAttribute("aria-valuetext", Math.round(value) + "% " + before + ", " + Math.round(100 - value) + "% " + after);
    }
    set(50);
    range.addEventListener("input", function () { set(Number(range.value)); });

    var dragging = false;
    function fromPointer(event) {
      var rect = box.getBoundingClientRect();
      set((event.clientX - rect.left) / rect.width * 100);
    }
    box.addEventListener("pointerdown", function (event) {
      if (event.button !== 0) return;
      dragging = true;
      fromPointer(event);
      try { box.setPointerCapture(event.pointerId); } catch (e) {}
    });
    box.addEventListener("pointermove", function (event) { if (dragging) fromPointer(event); });
    function stop() { dragging = false; }
    box.addEventListener("pointerup", stop);
    box.addEventListener("pointercancel", stop);
    box.addEventListener("lostpointercapture", stop);
    /* a tap or click on the picture also moves the keyboard focus to it */
    box.addEventListener("click", function () { range.focus({ preventScroll: true }); });

    figure.appendChild(box);
    caption(figure, item.caption);
    return figure;
  }

  /* A short muted loop. It plays while on screen, unless the visitor asks
     for less motion; the button pauses and plays it. */
  function video(item, eager) {
    var figure = el("figure", "clip");
    var box = frame(item);
    var v = el("video");
    v.muted = true;
    v.loop = true;
    v.playsInline = true;
    v.setAttribute("muted", "");
    v.setAttribute("playsinline", "");
    v.preload = eager ? "auto" : "none";
    if (item.poster) v.poster = item.poster;
    if (item.label) v.setAttribute("aria-label", item.label);
    if (item.webm) { var s1 = el("source"); s1.src = item.webm; s1.type = "video/webm"; v.appendChild(s1); }
    if (item.mp4) { var s2 = el("source"); s2.src = item.mp4; s2.type = "video/mp4"; v.appendChild(s2); }
    box.appendChild(v);

    var button = el("button", "clip-toggle");
    button.type = "button";
    var pauseIcon = "M6 5h4v14H6zM14 5h4v14h-4z";
    var playIcon = "M7 4v16l13-8z";
    var userPaused = reduceMotion && reduceMotion.matches;
    var visible = false;

    function render() {
      var playing = !v.paused;
      button.textContent = "";
      button.appendChild(icon(playing ? pauseIcon : playIcon));
      button.appendChild(document.createTextNode(playing ? "Pause" : "Play"));
      button.setAttribute("aria-label", (playing ? "Pause" : "Play") + (item.label ? ": " + item.label : " the clip"));
    }
    function update() {
      if (visible && !userPaused) {
        var p = v.play();
        if (p && p.catch) p.catch(function () {});
      } else {
        v.pause();
      }
    }
    button.addEventListener("click", function () {
      userPaused = !v.paused;
      if (!userPaused) visible = true;
      update();
    });
    v.addEventListener("play", render);
    v.addEventListener("pause", render);
    render();

    if ("IntersectionObserver" in window) {
      new IntersectionObserver(function (entries) {
        visible = entries[0].isIntersecting;
        update();
      }, { threshold: 0.25 }).observe(box);
    } else {
      visible = true;
      update();
    }
    box.appendChild(button);
    figure.appendChild(box);
    caption(figure, item.caption);
    return figure;
  }

  function picture(item) {
    var li = el("li");
    var link = el("a");
    link.href = item.full || item.src;
    var figure = el("figure");
    var box = el("div", "frame");
    box.appendChild(image(item.src, item.alt, item.width, item.height, false));
    figure.appendChild(box);
    caption(figure, item.caption);
    link.appendChild(figure);
    li.appendChild(link);
    return li;
  }

  /* the hero: the item whose id the list's "hero" names */
  var heroItem = null;
  if (media.hero) {
    compares.concat(videos).forEach(function (item) {
      if (item.id === media.hero) heroItem = item;
    });
  }
  if (heroItem) {
    var heroBox = document.getElementById("hero-media");
    heroBox.appendChild(compares.indexOf(heroItem) >= 0 ? compare(heroItem, true) : video(heroItem, true));
    heroBox.hidden = false;
    heroBox.closest(".hero").classList.add("has-media");
  }

  /* "See the difference": the pairs and the clips, not repeating the hero's */
  var seeList = document.getElementById("see-list");
  var shown = 0;
  compares.forEach(function (item) {
    if (item === heroItem) return;
    seeList.appendChild(compare(item, false));
    shown++;
  });
  videos.forEach(function (item) {
    if (item === heroItem) return;
    seeList.appendChild(video(item, false));
    shown++;
  });
  if (shown) document.getElementById("see").hidden = false;
  if (!seeList.querySelector(".compare")) document.getElementById("see-sub").hidden = true;

  var galleryList = document.getElementById("gallery-list");
  gallery.forEach(function (item) { galleryList.appendChild(picture(item)); });
  if (gallery.length) document.getElementById("gallery").hidden = false;
})();
