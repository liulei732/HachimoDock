import {chromium} from 'playwright';
import assert from 'node:assert/strict';
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER,headless:true});
try{
  const page=await browser.newPage({viewport:{width:960,height:880}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto('http://127.0.0.1:4203/new-components-preview.html?component=hop');
  const screen=page.locator('.screen');await screen.locator('[data-entity="puppy"]').waitFor();
  const entities=()=>screen.locator('[data-entity]').evaluateAll(es=>es.map(e=>e.outerHTML));
  const ready=await entities();
  await screen.getByRole('button',{name:'重开',exact:true}).click();
  await screen.getByRole('dialog').getByRole('button',{name:'取消',exact:true}).click();
  assert.deepEqual(await entities(),ready);
  const box=await screen.getByRole('button',{name:'按住开始',exact:true}).boundingBox();
  await page.mouse.move(box.x+box.width/2,box.y+box.height/2);await page.mouse.down();
  await page.waitForTimeout(200);assert.deepEqual(await entities(),ready,'no first-press reroll');
  await page.waitForTimeout(480);await page.mouse.up();await page.waitForTimeout(120);
  await screen.getByRole('button',{name:'重开',exact:true}).click();await screen.getByRole('dialog').waitFor();
  await page.waitForTimeout(60);const frozen=await entities();await page.waitForTimeout(800);
  assert.deepEqual(await entities(),frozen,'airborne confirmation freezes motion');
  await screen.getByRole('dialog').getByRole('button',{name:'取消',exact:true}).click();
  await page.waitForTimeout(1500);assert.equal(Number(await screen.locator('[data-entity="puppy"]').getAttribute('data-y')),4);
  const metric = (size, upperBound = Infinity) => screen.locator(`[data-label]:has(> rect[data-text-box][height="${size}"])`).evaluateAll(
    (labels, maxY) => labels.filter(label => Number(label.querySelector('rect[data-text-box]').getAttribute('y')) < maxY)
      .map(label => label.getAttribute('data-label')).filter(value => /^\d+$/.test(value)), upperBound);
  assert.deepEqual(await metric(44), ['1'], 'current score after landing');
  assert((await metric(24, 60)).includes('1'), 'best score after landing');
  await screen.getByRole('button',{name:'暂停',exact:true}).click();
  await screen.getByRole('button',{name:'重开',exact:true}).click();
  await screen.getByRole('dialog').getByRole('button',{name:'确认',exact:true}).click();
  assert.equal(Number(await screen.locator('[data-entity="puppy"]').getAttribute('data-x')),2);
  assert.deepEqual(await metric(44), ['0'], 'restart resets the current score');
  assert.deepEqual(await metric(24, 60), ['1'], 'restart retains the best score');
  await screen.screenshot({path:'new-components/qa/hop-restart-ready.png'});
  assert.deepEqual(errors,[]);console.log('PASS first-press board stability; restart button, cancel, airborne modal freeze/resume, paused restart, best retention, no page errors.');
}finally{await browser.close();}
