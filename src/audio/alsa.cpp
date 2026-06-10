#ifdef ALSA_ENABLED

#include "device_enum.h"

//********************************************************************************************************************

static void free_alsa(extAudio *Self)
{
   if (Self->sndlog) { snd_output_close(Self->sndlog); Self->sndlog = nullptr; }
   if (Self->Handle) { snd_pcm_close(Self->Handle); Self->Handle = nullptr; }
   if (Self->MixHandle) { snd_mixer_close(Self->MixHandle); Self->MixHandle = nullptr; }
   if (Self->AudioBuffer) { FreeResource(Self->AudioBuffer); Self->AudioBuffer = nullptr; }
}

//********************************************************************************************************************

static ERR init_audio(extAudio *Self)
{
   kt::Log log(__FUNCTION__);
   struct snd::SetVolume setvol;
   snd_pcm_hw_params_t *hwparams;
   snd_pcm_stream_t stream;
   snd_pcm_t *pcmhandle;
   snd_mixer_elem_t *elem;
   snd_mixer_selem_id_t *sid;
   snd_pcm_uframes_t periodsize;
   int err, index;
   int16_t channel;
   long pmin, pmax;
   int dir;
   std::string pcm_name;

   if (Self->Handle) {
      log.msg("Audio system is already active.");
      return ERR::Okay;
   }

   log.msg("Initialising sound card device.");

   // If 'plughw:0,0' is used, we get ALSA's software mixer, which allows us to set any kind of output options.
   // If 'hw:0,0' is used, we get precise hardware information.  Otherwise stick to 'default'.

   if (!Self->Device.empty()) pcm_name = Self->Device;
   else pcm_name = "default";

   // Use unified device enumeration to find the appropriate audio device
   ALSADeviceInfo selected_device;

   if (iequals("default", pcm_name)) {
      // Select best available device (most mixer controls, not a modem)
      selected_device = ALSADeviceEnumerator::select_best_device();
      if (selected_device.card_number IS -1) {
         log.warning("There are no sound cards supported by audio drivers.");
         return ERR::NoSupport;
      }

      Self->Device = selected_device.card_id;
      pcm_name = selected_device.device_name;
      log.msg("Selected default device: %s (%s) with %d mixer controls",
              selected_device.card_id.c_str(), selected_device.card_name.c_str(),
              selected_device.mixer_controls);
   }
   else {
      // Find specific device by ID
      selected_device = ALSADeviceEnumerator::find_device_by_id(pcm_name);
      if (selected_device.card_number IS -1) {
         log.warning("Requested device '%s' not found.", pcm_name.c_str());
         return ERR::NoSupport;
      }

      pcm_name = selected_device.device_name;
      log.msg("Using specified device: %s (%s)",
              selected_device.card_id.c_str(), selected_device.card_name.c_str());
   }

   snd_output_stdio_attach(&Self->sndlog, stderr, 0);

   // If a mix handle is open from a previous Activate() attempt, close it

   if (Self->MixHandle) {
      snd_mixer_close(Self->MixHandle);
      Self->MixHandle = nullptr;
   }

   // Mixer initialisation, for controlling volume

   if ((err = snd_mixer_open(&Self->MixHandle, 0)) < 0) {
      log.warning("snd_mixer_open() %s", snd_strerror(err));
      return ERR::Failed;
   }

   if ((err = snd_mixer_attach(Self->MixHandle, pcm_name.c_str())) < 0) {
      log.warning("snd_mixer_attach() %s", snd_strerror(err));
      return ERR::Failed;
   }

   if ((err = snd_mixer_selem_register(Self->MixHandle, nullptr, nullptr)) < 0) {
      log.warning("snd_mixer_selem_register() %s", snd_strerror(err));
      return ERR::Failed;
   }

   if ((err = snd_mixer_load(Self->MixHandle)) < 0) {
      log.warning("snd_mixer_load() %s", snd_strerror(err));
      return ERR::Failed;
   }

   // Build a list of all available volume controls

   snd_mixer_selem_id_alloca(&sid);
   int voltotal = 0;
   for (elem=snd_mixer_first_elem(Self->MixHandle); elem; elem=snd_mixer_elem_next(elem)) voltotal++;

   log.msg("%d mixer controls have been reported by alsa.", voltotal);

   if (voltotal < 1) {
      log.warning("Aborting due to lack of mixers for the sound device.");
      return ERR::NoSupport;
   }

   std::vector<VolumeCtl> volctl;
   volctl.reserve(32);

   index = 0;
   for (elem=snd_mixer_first_elem(Self->MixHandle); elem; elem=snd_mixer_elem_next(elem)) {
      volctl.resize(volctl.size() + 1);

      snd_mixer_selem_get_id(elem, sid);
      if (!snd_mixer_selem_is_active(elem)) continue;

      if ((volctl[index].Flags & VCF::CAPTURE) != VCF::NIL) {
         snd_mixer_selem_get_capture_volume_range(elem, &pmin, &pmax);
      }
      else snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);

      if (pmin >= pmax) continue; // Ignore mixers with no range

      log.trace("Mixer Control '%s',%i", snd_mixer_selem_id_get_name(sid), snd_mixer_selem_id_get_index(sid));

      volctl[index].Name = snd_mixer_selem_id_get_name(sid);

      for (channel=0; channel < (int)volctl[index].Channels.size(); channel++) volctl[index].Channels[channel] = -1;

      VCF flags = VCF::NIL;
      if (snd_mixer_selem_has_playback_volume(elem))        flags |= VCF::PLAYBACK;
      if (snd_mixer_selem_has_capture_volume(elem))         flags |= VCF::CAPTURE;
      if (snd_mixer_selem_has_capture_volume_joined(elem))  flags |= VCF::JOINED;
      if (snd_mixer_selem_has_playback_volume_joined(elem)) flags |= VCF::JOINED;
      if (snd_mixer_selem_is_capture_mono(elem))            flags |= VCF::MONO;
      if (snd_mixer_selem_is_playback_mono(elem))           flags |= VCF::MONO;

      // Get the current channel volumes

      volctl[index].Channels.resize(std::ssize(glAlsaConvert));
      if ((flags & VCF::MONO) IS VCF::NIL) {
         for (channel=0; channel < std::ssize(glAlsaConvert); channel++) {
            if (snd_mixer_selem_has_playback_channel(elem, (snd_mixer_selem_channel_id_t)glAlsaConvert[channel]))   {
               long vol;
               snd_mixer_selem_get_playback_volume(elem, (snd_mixer_selem_channel_id_t)glAlsaConvert[channel], &vol);
               volctl[index].Channels[channel] = vol;
            }
         }
      }
      else volctl[index].Channels[0] = 0;

      // By default, input channels need to be muted.  This is because some rare PC's have been noted to cause high
      // pitched feedback, e.g. when the microphone channel is on.  All playback channels are enabled by default.

      if ((snd_mixer_selem_has_capture_switch(elem)) and (!snd_mixer_selem_has_playback_switch(elem))) {
         for (channel=0; channel < std::ssize(glAlsaConvert); channel++) {
            flags |= VCF::MUTE;
            snd_mixer_selem_set_capture_switch(elem, (snd_mixer_selem_channel_id_t)channel, 0);
         }
      }
      else if (snd_mixer_selem_has_playback_switch(elem)) {
         for (channel=0; channel < std::ssize(glAlsaConvert); channel++) {
            snd_mixer_selem_set_capture_switch(elem, (snd_mixer_selem_channel_id_t)channel, 1);
         }
      }

      volctl[index].Flags = flags;

      index++;
   }

   log.msg("Configured %d mixer controls.", index);

   snd_pcm_hw_params_alloca(&hwparams); // Stack allocation, no need to free it

   stream = SND_PCM_STREAM_PLAYBACK;
   if ((err = snd_pcm_open(&pcmhandle, pcm_name.c_str(), stream, 0)) < 0) {
      log.warning("snd_pcm_open(%s) %s", pcm_name.c_str(), snd_strerror(err));
      return ERR::Failed;
   }

   // Set access type, either SND_PCM_ACCESS_RW_INTERLEAVED or SND_PCM_ACCESS_RW_NONINTERLEAVED.

   if ((err = snd_pcm_hw_params_any(pcmhandle, hwparams)) < 0) {
      log.warning("Broken configuration for this PCM: no configurations available");
      return ERR::Failed;
   }

   if ((err = snd_pcm_hw_params_set_access(pcmhandle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
      log.warning("set_access() %d %s", err, snd_strerror(err));
      return ERR::Failed;
   }

   // Set the preferred audio bit format

   if (Self->BitDepth IS 32) {
      if ((err = snd_pcm_hw_params_set_format(pcmhandle, hwparams, SND_PCM_FORMAT_FLOAT_LE)) < 0) {
         log.warning("set_format(32) %s", snd_strerror(err));
         return ERR::Failed;
      }
   }
   else if (Self->BitDepth IS 16) {
      if ((err = snd_pcm_hw_params_set_format(pcmhandle, hwparams, SND_PCM_FORMAT_S16_LE)) < 0) {
         log.warning("set_format(16) %s", snd_strerror(err));
         return ERR::Failed;
      }
   }
   else if ((err = snd_pcm_hw_params_set_format(pcmhandle, hwparams, SND_PCM_FORMAT_U8)) < 0) {
      log.warning("set_format(8) %s", snd_strerror(err));
      return ERR::Failed;
   }

   // Retrieve the bit rate from alsa

   snd_pcm_format_t bitformat;
   snd_pcm_hw_params_get_format(hwparams, &bitformat);

   switch (bitformat) {
      case SND_PCM_FORMAT_S16_LE:
      case SND_PCM_FORMAT_S16_BE:
      case SND_PCM_FORMAT_U16_LE:
      case SND_PCM_FORMAT_U16_BE:
         Self->BitDepth = 16;
         break;

      case SND_PCM_FORMAT_S8:
      case SND_PCM_FORMAT_U8:
         Self->BitDepth = 8;
         break;

      default:
         log.warning("Hardware uses an unsupported audio format.");
         return ERR::Failed;
   }

   log.msg("ALSA bit rate: %d", Self->BitDepth);

   // Set the output rate to the rate that we are using internally.  ALSA will use the nearest possible rate allowed
   // by the hardware.

   dir = 0;
   if ((err = snd_pcm_hw_params_set_rate_near(pcmhandle, hwparams, (uint32_t *)&Self->OutputRate, &dir)) < 0) {
      log.warning("set_rate_near() %s", snd_strerror(err));
      return ERR::Failed;
   }

   // Set number of channels

   uint32_t channels = ((Self->Flags & ADF::STEREO) != ADF::NIL) ? 2 : 1;
   if ((err = snd_pcm_hw_params_set_channels_near(pcmhandle, hwparams, &channels)) < 0) {
      log.warning("set_channels_near(%d) %s", channels, snd_strerror(err));
      return ERR::Failed;
   }

   if (channels IS 2) Self->Stereo = true;
   else Self->Stereo = false;

   snd_pcm_uframes_t periodsize_min, periodsize_max;
   snd_pcm_uframes_t buffersize_min, buffersize_max;

   snd_pcm_hw_params_get_buffer_size_min(hwparams, &buffersize_min);
   snd_pcm_hw_params_get_buffer_size_max(hwparams, &buffersize_max);

   dir = 0;
   snd_pcm_hw_params_get_period_size_min(hwparams, &periodsize_min, &dir);

   dir = 0;
   snd_pcm_hw_params_get_period_size_max(hwparams, &periodsize_max, &dir);

   // NOTE: Audio buffersize is measured in samples, not bytes

   snd_pcm_uframes_t buffersize = DEFAULT_BUFFER_SIZE;

   if (buffersize > buffersize_max) buffersize = buffersize_max;
   else if (buffersize < buffersize_min) buffersize = buffersize_min;

   periodsize = buffersize / 4;
   if (periodsize < periodsize_min) periodsize = periodsize_min;
   else if (periodsize > periodsize_max) periodsize = periodsize_max;
   buffersize = periodsize * 4;

   // Set buffer sizes.  Note that we will retrieve the period and buffer sizes AFTER telling ALSA what the audio
   // parameters are.

   log.msg("Using period frame size of %d, buffer size of %d", (int)periodsize, (int)buffersize);

   if ((err = snd_pcm_hw_params_set_period_size_near(pcmhandle, hwparams, &periodsize, 0)) < 0) {
      log.warning("Period size failure: %s", snd_strerror(err));
      return ERR::Failed;
   }

   if ((err = snd_pcm_hw_params_set_buffer_size_near(pcmhandle, hwparams, &buffersize)) < 0) {
      log.warning("Buffer size failure: %s", snd_strerror(err));
      return ERR::Failed;
   }

   // ALSA device initialisation

   if ((err = snd_pcm_hw_params(pcmhandle, hwparams)) < 0) {
      log.warning("snd_pcm_hw_params() %s", snd_strerror(err));
      return ERR::Failed;
   }

   if ((err = snd_pcm_prepare(pcmhandle)) < 0) {
      log.warning("snd_pcm_prepare() %s", snd_strerror(err));
      return ERR::Failed;
   }

   // Retrieve ALSA buffer sizes

   err = snd_pcm_hw_params_get_periods(hwparams, (uint32_t *)&Self->Periods, &dir);

   snd_pcm_hw_params_get_period_size(hwparams, &periodsize, 0);
   Self->PeriodSize = periodsize;

   // Note that ALSA reports the audio buffer size in samples, not bytes

   snd_pcm_hw_params_get_buffer_size(hwparams, &buffersize);
   Self->AudioBufferSize = BYTELEN(buffersize);

   if (Self->Stereo) Self->AudioBufferSize = BYTELEN(Self->AudioBufferSize<<1);
   Self->AudioBufferSize = BYTELEN(Self->AudioBufferSize * (Self->BitDepth/8));

   log.msg("Total Periods: %d, Period Size: %d, Buffer Size: %d (bytes)", Self->Periods, Self->PeriodSize, Self->AudioBufferSize);

   // Allocate a buffer that we will use for audio output

   if (Self->AudioBuffer) { FreeResource(Self->AudioBuffer); Self->AudioBuffer = nullptr; }

   if (AllocMemory(Self->AudioBufferSize, MEM::DATA, (APTR *)&Self->AudioBuffer) IS ERR::Okay) {
      if ((Self->Flags & ADF::SYSTEM_WIDE) != ADF::NIL) {
         log.msg("Applying user configured volumes.");

         auto oldctl = Self->Volumes;
         Self->Volumes = volctl;

         for (int i=0; i < (int)volctl.size(); i++) {
            int j;
            for (j=0; j < (int)oldctl.size(); j++) {
               if (volctl[i].Name == oldctl[j].Name) {
                  setvol.Index   = i;
                  setvol.Name    = std::string_view{};
                  setvol.Flags   = SVF::NIL;
                  setvol.Channel = -1;
                  setvol.Volume  = oldctl[j].Channels[0];
                  if ((oldctl[j].Flags & VCF::MUTE) != VCF::NIL) setvol.Flags |= SVF::MUTE;
                  else setvol.Flags |= SVF::UNMUTE;
                  Action(snd::SetVolume::id, Self, &setvol);
                  break;
               }
            }

            // If the user has no volume defined for a mixer, set our own.

            if (j IS (int)oldctl.size()) {
               setvol.Index   = i;
               setvol.Name    = std::string_view{};
               setvol.Flags   = SVF::NIL;
               setvol.Channel = -1;
               setvol.Volume  = 0.8;
               Action(snd::SetVolume::id, Self, &setvol);
            }
         }
      }
      else {
         log.msg("Skipping preset volumes.");
         Self->Volumes = volctl;
      }

      // Free existing volume measurements and apply the information that we read from alsa.

      Self->Handle = pcmhandle;
   }
   else {
      return log.warning(ERR::AllocMemory);
   }

   return ERR::Okay;
}

#endif
