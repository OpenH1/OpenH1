// The review app in a real browser, on the made-up gallery of tests/fixture.py: selection, drilling in and out,
// zoom and pan, a variant switch keeping the selection, a drag snapped to a neighbour giving the right delta,
// a comment made, replied to and resolved, and a phone's touches.
import { expect, Page, test } from "@playwright/test";

const AUDIO = "main_menu/settings_select/player_setup/player_profile_edit/audio_settings/audio_settings_screen";
const screenUrl = (variant = "keyboard-16x9", run = "latest") =>
  `login?key=test&next=${encodeURIComponent(`./#/s/${run}/${encodeURIComponent(AUDIO)}/${variant}`)}`;

/** a point in the menus' units in the page's pixels */
async function at(page: Page, x: number, y: number): Promise<[number, number]> {
  return page.evaluate(([mx, my]) => (window as any).menuReview.view().clientPoint(mx, my), [x, y]);
}

async function open(page: Page, variant = "keyboard-16x9", run = "latest") {
  await page.goto(screenUrl(variant, run));
  await expect(page.locator(".viewport.main")).toBeVisible();
  await expect(page.locator(".layers, .inspector").first()).toBeVisible();
  await expect(page.locator(".viewport.main[data-placed]")).toHaveCount(1);
}

async function inspected(page: Page) {
  await page.locator('.tabs [data-tab="inspect"]').click();
  return page.locator(".inspector h3");
}

test("a click selects the widget under it, double-click drills in and Esc goes out", async ({ page }) => {
  await open(page);
  await page.mouse.move(...(await at(page, 200, 130)));
  await expect(page.locator(".overlay .hover")).toHaveCount(1);
  await page.mouse.click(...(await at(page, 200, 130)));
  await expect(await inspected(page)).toHaveText("label_a");
  await page.keyboard.press("Escape");
  await expect(await inspected(page)).toHaveText("panel");
  await page.mouse.dblclick(...(await at(page, 200, 130)));
  await expect(await inspected(page)).toHaveText("label_a");
  // (shift adds to the selection; the layers show both)
  await page.keyboard.down("Shift");
  await page.mouse.click(...(await at(page, 500, 410)));
  await page.keyboard.up("Shift");
  await expect(page.locator(".inspector h3").first()).toHaveText("2 widgets");
  await page.locator('.tabs [data-tab="layers"]').click();
  await expect(page.locator(".layer.selected")).toHaveCount(2);
  await page.locator(".layer", { hasText: "label_b" }).click();
  await expect(page.locator(".layer.selected")).toHaveText(/label_b/);
});

test("zoom by keys and Ctrl-scroll, pan by space-drag", async ({ page }) => {
  await open(page);
  const viewport = page.locator(".viewport.main");
  const zoom = async () => Number(await viewport.getAttribute("data-zoom"));
  const first = await zoom();
  await viewport.focus();
  await page.keyboard.press("+");
  expect(await zoom()).toBeGreaterThan(first);
  const box = (await viewport.boundingBox())!;
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.keyboard.down("Control");
  await page.mouse.wheel(0, -200);
  await page.keyboard.up("Control");
  const zoomed = await zoom();
  expect(zoomed).toBeGreaterThan(first * 1.3);
  const before = await at(page, 200, 130);
  await page.keyboard.down(" ");
  await page.mouse.down();
  await page.mouse.move(box.x + box.width / 2 + 120, box.y + box.height / 2 + 40, { steps: 5 });
  await page.mouse.up();
  await page.keyboard.up(" ");
  const after = await at(page, 200, 130);
  expect(Math.round(after[0] - before[0])).toBe(120);
  expect(Math.round(after[1] - before[1])).toBe(40);
  await expect(page.locator(".viewport.main .stage.crisp")).toHaveCount(zoomed > 1 ? 1 : 0);
});

test("a variant switch keeps the screen, the selection and the view", async ({ page }) => {
  await open(page);
  await page.mouse.click(...(await at(page, 200, 130)));
  await expect(await inspected(page)).toHaveText("label_a");
  const [ax, ay] = await at(page, 120, 120);
  const [bx] = await at(page, 300, 140);
  await page.locator(".viewport.main").focus();
  await page.keyboard.press("]");
  await expect(page).toHaveURL(/controller-16x10/);
  await expect(await inspected(page)).toHaveText("label_a");
  const [cx, cy] = await at(page, 120, 120);
  const [dx] = await at(page, 300, 140);
  expect(Math.abs(dx - cx - (bx - ax))).toBeLessThan(1.5);
  expect(Math.abs(cx - ax)).toBeLessThan(40);
  expect(Math.abs(cy - ay)).toBeLessThan(40);
});

test("a drag in Edit snaps to a neighbour's edge and its note records the delta", async ({ page, request }) => {
  await open(page);
  await page.locator(".viewport.main").focus();
  await page.keyboard.press("e");
  await page.mouse.click(...(await at(page, 200, 170)));
  await expect(await inspected(page)).toHaveText("label_b");
  // (dragged up 17 units: its top at 143, three from label_a's foot at 140, where it snaps)
  const [sx, sy] = await at(page, 200, 170);
  const [, ty] = await at(page, 200, 153);
  await page.mouse.move(sx, sy);
  await page.mouse.down();
  await page.mouse.move(sx, (sy + ty) / 2, { steps: 4 });
  await page.mouse.move(sx, ty, { steps: 4 });
  // (two guides: its left edge on label_a's, its top on label_a's foot)
  await expect(page.locator(".overlay .guide")).toHaveCount(2);
  await page.mouse.up();
  await expect(page.locator(".proposal")).toContainText("Δ 0, -20");
  // (arrows nudge, and undo takes it back)
  await page.keyboard.press("ArrowRight");
  await expect(page.locator(".proposal")).toContainText("Δ +1, -20");
  await page.keyboard.press(process.platform === "darwin" ? "Meta+z" : "Control+z");
  await expect(page.locator(".proposal")).toContainText("Δ 0, -20");
  await page.locator(".proposal button.primary").click();
  await page.locator(".composer textarea").fill("Close the gap");
  await page.locator(".composer button.primary").click();
  await expect(page.locator(".composer")).toHaveCount(0);
  const notes = await (await request.get("api/notes", { headers: { Cookie: await cookie(page) } })).json();
  const note = notes[notes.length - 1];
  expect(note.edits.box.delta).toEqual([0, -20, 0, 0]);
  expect(note.edits.box.to).toEqual([120, 140, 300, 160]);
  expect(note.widget).toMatch(/label_b$/);
});

test("a comment is made in comment mode, replied to and resolved", async ({ page, request }) => {
  await open(page);
  await page.locator(".viewport.main").focus();
  await page.keyboard.press("c");
  await page.mouse.click(...(await at(page, 500, 410)));
  await page.locator(".composer textarea").fill("Is OK the right word here?");
  await page.locator(".composer select").first().selectOption("question");
  await page.locator(".composer select").nth(1).selectOption("screen");
  await page.locator(".composer input").fill("Paul");
  await page.locator(".composer button.primary").click();
  const card = page.locator(".note", { hasText: "Is OK the right word here?" });
  await expect(card).toBeVisible();
  await expect(page.locator('.pin[title*="Is OK"]')).toHaveCount(1);
  await card.locator("textarea.reply").fill("Yes: it saves the settings.");
  await card.getByRole("button", { name: "Reply" }).click();
  await expect(card.locator(".thread")).toContainText("it saves the settings");
  await card.getByRole("button", { name: "Resolve" }).click();
  await expect(page.locator('.pin[title*="Is OK"]')).toHaveCount(0);
  const notes = await (await request.get("api/notes", { headers: { Cookie: await cookie(page) } })).json();
  const note = notes.find((each: any) => each.note === "Is OK the right word here?");
  expect(note.status).toBe("closed");
  expect(note.kind).toBe("question");
  expect(note.scope).toBe("screen");
  expect(note.thread[0].text).toContain("saves the settings");
  expect(note.author).toBe("Paul");
});

test("a pull request's run shows its before beside, swiped and as an onion skin", async ({ page }) => {
  await open(page, "keyboard-16x9", "pr-1");
  await page.locator('[data-compare="side"]').click();
  await expect(page.locator(".viewport.side-before")).toBeVisible();
  await page.locator('[data-compare="swipe"]').click();
  await expect(page.locator(".before-overlay")).toBeVisible();
  await page.locator('[data-compare="onion"]').click();
  await page.locator(".compare-slider").fill("30");
  await expect(page.locator(".before-overlay")).toHaveCSS("opacity", "0.3");
});

test.describe("a phone", () => {
  test.use({ viewport: { width: 390, height: 844 }, hasTouch: true, isMobile: true });

  async function touch(page: Page, type: string, id: number, x: number, y: number) {
    await page.locator(".viewport.main").dispatchEvent(type, {
      pointerId: id, pointerType: "touch", clientX: x, clientY: y, isPrimary: id === 1, bubbles: true, button: 0, buttons: 1,
    });
  }

  test("a tap selects, a long press comments, a pinch zooms", async ({ page }) => {
    await open(page);
    const [x, y] = await at(page, 200, 130);
    await touch(page, "pointerdown", 1, x, y);
    await touch(page, "pointerup", 1, x, y);
    await expect(await inspected(page)).toHaveText("label_a");
    const [bx, by] = await at(page, 500, 410);
    await touch(page, "pointerdown", 1, bx, by);
    await page.waitForTimeout(800);
    await touch(page, "pointerup", 1, bx, by);
    await expect(page.locator(".composer")).toBeVisible();
    await expect(page.locator(".composer-title")).toContainText("button_ok");
    await page.locator(".composer").getByRole("button", { name: "Cancel" }).click();
    const viewport = page.locator(".viewport.main");
    const first = Number(await viewport.getAttribute("data-zoom"));
    const box = (await viewport.boundingBox())!;
    const cx = box.x + box.width / 2;
    const cy = box.y + box.height / 2;
    await touch(page, "pointerdown", 1, cx - 20, cy);
    await touch(page, "pointerdown", 2, cx + 20, cy);
    for (const spread of [40, 70, 100]) {
      await touch(page, "pointermove", 1, cx - spread, cy);
      await touch(page, "pointermove", 2, cx + spread, cy);
    }
    await touch(page, "pointerup", 1, cx - 100, cy);
    await touch(page, "pointerup", 2, cx + 100, cy);
    expect(Number(await viewport.getAttribute("data-zoom"))).toBeGreaterThan(first * 3);
  });
});

async function cookie(page: Page): Promise<string> {
  return (await page.context().cookies()).map((each) => `${each.name}=${each.value}`).join("; ");
}
