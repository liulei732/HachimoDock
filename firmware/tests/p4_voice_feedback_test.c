#include <assert.h>
#include <string.h>
#include "pet_p4_voice_feedback.h"

static unsigned reference_level(const int16_t *pcm) {
  uint64_t energy = 0;
  for (unsigned i = 0; i < PET_VOICE_PCM_SAMPLES; ++i) {
    int32_t sample = pcm[i];
    energy += (uint32_t)(sample * sample);
  }
  uint64_t mean = energy / PET_VOICE_PCM_SAMPLES;
  if (mean >= 1000000) return 100;
  unsigned rms = 0;
  while ((rms + 1) * (rms + 1) <= mean) ++rms;
  return rms <= 20 ? 0 : (rms - 20) * 100 / 980;
}

static void test_draft_confirmation_window(void) {
  pet_voice_meter_t meter = {.capture_id = "cap"};
  pet_voice_feedback_t draft = {0};
  pet_voice_feedback_sync(&draft, &meter, 100);
  assert(!pet_voice_feedback_can_confirm(&draft, 100));
  assert(pet_voice_feedback_update(&draft, "cap", "utterance", 1, "draft_ready", "text", "", 200));
  assert(pet_voice_feedback_can_confirm(&draft, 15199));
  assert(!pet_voice_feedback_can_confirm(&draft, 15200));

  pet_voice_feedback_t expired = draft;
  pet_voice_feedback_sync(&expired, &meter, 15200);
  assert(expired.phase == PET_VOICE_IDLE);
  assert(!pet_voice_feedback_can_confirm(&expired, 15201));
  assert(!pet_voice_feedback_update(&expired, "cap", "utterance", 1, "submitted", "text", "", 15201));

  pet_voice_feedback_confirm_requested(&draft, 15199);
  assert(!pet_voice_feedback_can_confirm(&draft, 15199));
  pet_voice_feedback_sync(&draft, &meter, 15200);
  assert(draft.phase == PET_VOICE_SUBMITTING);
  assert(!pet_voice_feedback_update(&draft, "cap", "utterance", 1, "draft_ready", "text", "", 15201));
  assert(pet_voice_feedback_update(&draft, "cap", "utterance", 1, "submitted", "text", "", 15202));
  assert(draft.phase == PET_VOICE_SUBMITTED);
}

int main(void) {
  test_draft_confirmation_window();
  int16_t silence[PET_VOICE_PCM_SAMPLES] = {0}, quiet[PET_VOICE_PCM_SAMPLES], loud[PET_VOICE_PCM_SAMPLES];
  for (unsigned i=0; i<PET_VOICE_PCM_SAMPLES; ++i) { quiet[i]=(i%2?200:-200); loud[i]=(i%2?2000:-2000); }
  assert(pet_voice_pcm_level(silence)==0);
  assert(pet_voice_pcm_level(quiet)>0);
  assert(pet_voice_pcm_level(quiet)<pet_voice_pcm_level(loud));
  for (unsigned i=0; i<PET_VOICE_PCM_SAMPLES; ++i) loud[i]=INT16_MIN;
  assert(pet_voice_pcm_level(loud)==100);
  /* Mixed amplitudes exercise floor rounding and the saturation boundary. */
  for (int amplitude = 0; amplitude <= 1500; ++amplitude) {
    for (unsigned i = 0; i < PET_VOICE_PCM_SAMPLES; ++i) {
      loud[i] = i % 3 ? (int16_t)-amplitude : (int16_t)(amplitude / 3);
    }
    assert(pet_voice_pcm_level(loud) == reference_level(loud));
  }
  silence[PET_VOICE_PCM_SAMPLES - 1] = INT16_MIN;
  assert(pet_voice_pcm_level(silence) == reference_level(silence));
  pet_voice_meter_t m={0}; strcpy(m.capture_id,"capture-1");m.active=true;m.started_ms=100;
  for (unsigned i=0;i<12;++i) pet_voice_meter_sample(&m,100,100+i*50);
  assert(m.level>90 && m.bars[PET_VOICE_BARS-1]>90);
  for (unsigned i=0;i<45;++i) pet_voice_meter_sample(&m,0,700+i*50);
  assert(m.level==0);
  for (unsigned i=0;i<PET_VOICE_BARS;++i) assert(m.bars[i]==0);

  pet_voice_feedback_t v={0}; pet_voice_feedback_sync(&v,&m,3000);
  assert(v.phase==PET_VOICE_LISTENING);
  assert(pet_voice_feedback_update(&v,"capture-1","utterance-1",2,"partial","今天的额度","",3001));
  assert(!pet_voice_feedback_update(&v,"capture-old","utterance-1",3,"error","过期","",3002));
  assert(!pet_voice_feedback_update(&v,"capture-1","wrong-owner",3,"partial","错误","",3002));
  assert(!pet_voice_feedback_update(&v,"capture-1","utterance-1",1,"partial","旧文本","",3002));
  m.active=false;pet_voice_feedback_sync(&v,&m,3100);
  assert(v.phase==PET_VOICE_RECOGNIZING && !strcmp(v.text,"今天的额度"));
  assert(pet_voice_feedback_update(&v,"capture-1","utterance-1",3,"partial","今天的 Codex 额度","",3101));
  assert(v.phase==PET_VOICE_RECOGNIZING); // Release never regresses to listening.
  assert(pet_voice_feedback_update(&v,"capture-1","utterance-1",3,"draft_ready","今天的 Codex 额度","",3102));
  assert(v.phase==PET_VOICE_DRAFT && strstr(pet_voice_feedback_title(&v),"草稿"));
  v.draft_inserted=true;assert(strstr(pet_voice_feedback_title(&v),"已写入"));
  assert(!pet_voice_feedback_update(&v,"capture-1","utterance-1",4,"partial","迟到","",3103));
  assert(pet_voice_feedback_update(&v,"capture-1","utterance-1",3,"error","","请保留目标窗口",3104));
  assert(strstr(v.text,"Codex") && v.phase==PET_VOICE_ERROR);
  v.failure_stage=PET_VOICE_FAILURE_SUBMISSION;
  assert(!strcmp(pet_voice_feedback_title(&v),"发送失败"));
  v.failure_stage=PET_VOICE_FAILURE_INSERTION;
  assert(!strcmp(pet_voice_feedback_title(&v),"写入失败"));
  v.failure_stage=PET_VOICE_FAILURE_RECOGNITION;
  assert(!strcmp(pet_voice_feedback_title(&v),"识别失败"));
  assert(!pet_voice_feedback_update(&v,"capture-1","utterance-1",4,"draft_ready","误报成功","",3105));

  strcpy(m.capture_id,"capture-2");m.active=true;m.started_ms=4000;
  pet_voice_feedback_sync(&v,&m,4000);
  assert(v.phase==PET_VOICE_LISTENING && !v.text[0] && !v.utterance_id[0]);
  assert(!pet_voice_feedback_update(&v,"capture-1","utterance-1",10,"submitted","旧结果","",4001));
  m.active=false;pet_voice_feedback_sync(&v,&m,4100);
  pet_voice_feedback_sync(&v,&m,19100);
  assert(v.phase==PET_VOICE_UNKNOWN && strstr(v.hint,"未确认"));
  pet_voice_feedback_sync(&v,&m,29100);
  assert(v.phase==PET_VOICE_IDLE && !v.text[0]);
  pet_voice_feedback_sync(&v,&m,30000);
  assert(v.phase==PET_VOICE_IDLE); // An old meter snapshot cannot resurrect an expired overlay.

  char text[8]; pet_voice_text_copy(text,sizeof(text),"语音输入");
  assert(!strcmp(text,"语音"));
  return 0;
}
