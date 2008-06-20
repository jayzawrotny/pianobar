/*
Copyright (c) 2008 Lars-Dominik Braun

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <piano.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ao/ao.h>
#include <neaacdec.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <readline/readline.h>

#include "terminal.h"
#include "settings.h"
#include "config.h"

struct aacPlayer {
	/* buffer; should be large enough */
	char buffer[CURL_MAX_WRITE_SIZE*2];
	size_t bufferFilled;
	/* got mdat atom */
	char dataMode;
	char foundEsdsAtom;
	char audioInitialized;
	/* aac */
	NeAACDecHandle aacHandle;
	unsigned long samplerate;
	unsigned char channels;
	/* audio out */
	ao_device *audioOutDevice;
	char *url;
	char finishedPlayback;
	char doQuit;
};

void dumpBuffer (char *buf, size_t len) {
	int i;
	for (i = 0; i < len; i++) {
		printf ("%02x ", buf[i] & 0xff);
	}
	printf ("\n");
}

/* FIXME: this is a huge block of _bad_ and buggy code */
size_t playCurlCb (void *ptr, size_t size, size_t nmemb, void *stream) {
	char *data = ptr;
	struct aacPlayer *player = stream;

	if (player->doQuit) {
		return 0;
	}

	/* fill buffer */
	if (player->bufferFilled + size*nmemb > sizeof (player->buffer)) {
		printf ("Buffer overflow!\n");
		return 0;
	}
	memcpy (player->buffer+player->bufferFilled, data, size*nmemb);
	player->bufferFilled += size*nmemb;

	if (player->dataMode == 1) {
		char *aacDecoded;
		NeAACDecFrameInfo frameInfo;

		/* 500 is just a guessed size; prevent buffer underruns and error
		 * messages by faad2 */
		while (player->bufferFilled > 500) {
			/* FIXME: well, i think we need this block length table */
			aacDecoded = NeAACDecDecode(player->aacHandle, &frameInfo,
					(unsigned char *) player->buffer, player->bufferFilled);
			if (frameInfo.error != 0) {
				printf ("error: %s\n\n",
						NeAACDecGetErrorMessage (frameInfo.error));
				break;
			}
			ao_play (player->audioOutDevice, aacDecoded,
					frameInfo.samples*frameInfo.channels);
			/* move remaining bytes to buffer beginning */
			memmove (player->buffer, player->buffer + frameInfo.bytesconsumed,
					player->bufferFilled - frameInfo.bytesconsumed);
			player->bufferFilled -= frameInfo.bytesconsumed;
		}
	} else {
		char *searchBegin = player->buffer;

		if (!player->foundEsdsAtom) {
			while (searchBegin < player->buffer + nmemb) {
				if (memcmp (searchBegin, "esds", 4) == 0) {
					player->foundEsdsAtom = 1;
					break;
				}
				searchBegin++;
			}
		}
		if (player->foundEsdsAtom && !player->audioInitialized) {
			/* FIXME: is this the correct way? */
			while (searchBegin < player->buffer + nmemb) {
				if (memcmp (searchBegin, "\x05\x80\x80\x80", 4) == 0) {
					ao_sample_format format;
					int audioOutDriver;

					/* +1+4 needs to be replaced by <something>! */
					char err = NeAACDecInit2 (player->aacHandle,
							(unsigned char *) searchBegin+1+4, 5,
							&player->samplerate, &player->channels);
					if (err != 0) {
						printf ("whoops... %i\n", err);
						return 1;
					}
					audioOutDriver = ao_default_driver_id();
					format.bits = 16;
					format.channels = player->channels;
					format.rate = player->samplerate;
					format.byte_format = AO_FMT_LITTLE;
					player->audioOutDevice = ao_open_live (audioOutDriver,
							&format, NULL);
					player->audioInitialized = 1;
					break;
				}
				searchBegin++;
			}
		}
		if (player->audioInitialized) {
			while (searchBegin < player->buffer + nmemb) {
				if (memcmp (searchBegin, "mdat", 4) == 0) {
					player->dataMode = 1;
					memmove (player->buffer, searchBegin + strlen ("mdat"),
							nmemb - (searchBegin - player->buffer));
					player->bufferFilled = nmemb - (searchBegin-player->buffer);
					break;
				}
				searchBegin++;
			}
		}
		if (!player->dataMode) {
			/* copy last four bytes to buffer beginning and set filled
			 * size to four */
			memcpy (player->buffer, player->buffer+player->bufferFilled - 4, 4);
			player->bufferFilled = 4;
		}
	}

	return size*nmemb;
}

/* player thread; for every song a new thread is started */
void *threadPlayUrl (void *data) {
	struct aacPlayer *player = data;
	NeAACDecConfigurationPtr conf;
	CURL *audioFd;

	audioFd = curl_easy_init ();
	player->aacHandle = NeAACDecOpen();

	conf = NeAACDecGetCurrentConfiguration(player->aacHandle);
	conf->outputFormat = FAAD_FMT_16BIT;
    conf->downMatrix = 1;
	NeAACDecSetConfiguration(player->aacHandle, conf);

	curl_easy_setopt (audioFd, CURLOPT_URL, player->url);
	curl_easy_setopt (audioFd, CURLOPT_WRITEFUNCTION, playCurlCb);
	curl_easy_setopt (audioFd, CURLOPT_WRITEDATA, player);
	curl_easy_setopt (audioFd, CURLOPT_USERAGENT, PACKAGE_STRING);
	curl_easy_perform (audioFd);

	NeAACDecClose(player->aacHandle);
	ao_close(player->audioOutDevice);
	curl_easy_cleanup (audioFd);

	player->finishedPlayback = 1;

	return NULL;
}

PianoStation_t *selectStation (PianoHandle_t *ph) {
	PianoStation_t *curStation;
	size_t i;

	printf ("which station do you want to listen to?\n");
	i = 0;
	curStation = ph->stations;
	while (curStation != NULL) {
		printf ("%2i) %s\n", i, curStation->name);
		curStation = curStation->next;
		i++;
	}
	scanf ("%i", &i);
	curStation = ph->stations;
	while (curStation != NULL && i > 0) {
		curStation = curStation->next;
		i--;
	}
	return curStation;
}

PianoSong_t *selectSong (PianoSong_t *startSong) {
	PianoSong_t *tmpSong;
	size_t i;

	tmpSong = startSong;
	i = 0;
	while (tmpSong != NULL) {
		printf ("%2u) %s - %s\n", i, tmpSong->artist, tmpSong->title);
		i++;
		tmpSong = tmpSong->next;
	}
	scanf ("%i", &i);
	tmpSong = startSong;
	while (tmpSong != NULL && i > 0) {
		tmpSong = tmpSong->next;
		i--;
	}
	return tmpSong;
}

PianoArtist_t *selectArtist (PianoArtist_t *startArtist) {
	PianoArtist_t *tmpArtist;
	size_t i;

	tmpArtist = startArtist;
	i = 0;
	while (tmpArtist != NULL) {
		printf ("%2u) %s\n", i, tmpArtist->name);
		i++;
		tmpArtist = tmpArtist->next;
	}
	scanf ("%i", &i);
	tmpArtist = startArtist;
	while (tmpArtist != NULL && i > 0) {
		tmpArtist = tmpArtist->next;
		i--;
	}
	return tmpArtist;
}

char *selectMusicId (PianoHandle_t *ph) {
	char *musicId = NULL, *lineBuf;
	char yesnoBuf;
	PianoSearchResult_t searchResult;
	PianoArtist_t *tmpArtist;
	PianoSong_t *tmpSong;

	lineBuf = readline ("Search for artist/title\n");
	if (lineBuf != NULL && strlen (lineBuf) > 0) {
		PianoSearchMusic (ph, lineBuf, &searchResult);
		if (searchResult.songs != NULL && searchResult.artists != NULL) {
			printf ("Is this an [a]rtist or [t]rack name?\n");
			read (fileno (stdin), &yesnoBuf, sizeof (yesnoBuf));
			if (yesnoBuf == 'a') {
				tmpArtist = selectArtist (searchResult.artists);
				musicId = strdup (tmpArtist->musicId);
				printf ("Ok.\n");
			} else if (yesnoBuf == 't') {
				tmpSong = selectSong (searchResult.songs);
				musicId = strdup (tmpSong->musicId);
				printf ("Ok.\n");
			}
		} else if (searchResult.songs != NULL) {
			printf ("Select song\n");
			tmpSong = selectSong (searchResult.songs);
			musicId = strdup (tmpSong->musicId);
			printf ("Ok.\n");
		} else if (searchResult.artists != NULL) {
			printf ("Select artist\n");
			tmpArtist = selectArtist (searchResult.artists);
			musicId = strdup (tmpArtist->musicId);
			printf ("Ok.\n");
		} else {
			printf ("Nothing found...\n");
		}
		PianoDestroySearchResult (&searchResult);
	}
	if (lineBuf != NULL) {
		free (lineBuf);
	}

	return musicId;
}

int main (int argc, char **argv) {
	PianoHandle_t ph;
	struct aacPlayer player;
	char doQuit = 0;
	PianoSong_t *curSong = NULL;
	PianoStation_t *curStation = NULL;
	BarSettings_t bsettings;
	pthread_t playerThread;

	printf ("Welcome to " PACKAGE_STRING "! Press ? for help.\n");

	/* init some things */
	curl_global_init (CURL_GLOBAL_SSL);
	xmlInitParser ();
	ao_initialize();

	BarSettingsInit (&bsettings);
	readSettings (&bsettings);

	if (bsettings.username == NULL) {
		bsettings.username = readline ("Username: ");
	}
	if (bsettings.password == NULL) {
		termSetEcho (0);
		bsettings.password = readline ("Password: ");
		termSetEcho (1);
	}

	PianoInit (&ph);
	/* setup control connection */
	if (bsettings.controlProxy != NULL &&
			bsettings.controlProxyType != -1) {
		curl_easy_setopt (ph.curlHandle, CURLOPT_PROXY,
				bsettings.controlProxy);
		curl_easy_setopt (ph.curlHandle, CURLOPT_PROXYTYPE,
				bsettings.controlProxyType);
	}
	curl_easy_setopt (ph.curlHandle, CURLOPT_CONNECTTIMEOUT, 60);

	termSetBuffer (0);

	printf ("Login...\n");
	if (PianoConnect (&ph, bsettings.username, bsettings.password) !=
			PIANO_RET_OK) {
		printf ("Login failed. Check your username and password\n");
		return 0;
	}
	printf ("Get stations...\n");
	if (PianoGetStations (&ph) != PIANO_RET_OK) {
		printf ("Error while fetching your stations.\n");
		return 0;
	}

	/* select station */
	curStation = selectStation (&ph);
	printf ("Playing station \"%s\"\n", curStation->name);

	/* little hack, needed to signal: hey! we need a playlist, but don't
	 * free anything (there is nothing to be freed yet) */
	memset (&player, 0, sizeof (player));
	player.finishedPlayback = 1;

	while (!doQuit) {
		/* check whether player finished playing and start playing new
		 * song */
		if (player.finishedPlayback == 1) {
			/* already played a song, clean up things */
			if (player.url != NULL) {
				free (player.url);
				memset (&player, 0, sizeof (player));
				pthread_join (playerThread, NULL);
			}

			if (curStation != NULL) {
				/* what's next? */
				if (curSong != NULL) {
					curSong = curSong->next;
				}
				if (curSong == NULL && curStation != NULL) {
					printf ("Receiving new playlist\n");
					PianoDestroyPlaylist (&ph);
					PianoGetPlaylist (&ph, curStation->id);
					curSong = ph.playlist;
					if (curSong == NULL) {
						printf ("No tracks left\n");
					}
				}
				if (curSong != NULL) {
					printf ("\"%s\" by \"%s\"%s\n", curSong->title,
							curSong->artist, (curSong->rating ==
							PIANO_RATE_LOVE) ? " (Loved)" : "");
					/* FIXME: why do we need to zero everything again? */
					memset (&player, 0, sizeof (player));
					player.url = strdup (curSong->audioUrl);
		
					/* start player */
					pthread_create (&playerThread, NULL, threadPlayUrl,
							&player);
				}
			}
		}

		/* in the meantime: wait for user actions */
		struct pollfd polls = {fileno (stdin), POLLIN, POLLIN};
		char buf, yesnoBuf;
		char *lineBuf, *musicId;

		if (poll (&polls, 1, 1000) > 0) {
			read (fileno (stdin), &buf, sizeof (buf));
			switch (buf) {
				case '?':
					printf ("a\tadd music to current station\n"
							"b\tban current song\n"
							"c\tcreate new station\n"
							"d\tdelete current station\n"
							"l\tlove current song\n"
							"n\tnext song\n"
							"q\tquit\n"
							"r\trename current station\n"
							"s\tchange station\n");
					break;

				case 'a':
					musicId = selectMusicId (&ph);
					if (PianoStationAddMusic (&ph, curStation, musicId) ==
							PIANO_RET_OK) {
						printf ("Added music to station.\n");
					} else {
						printf ("Error while adding music to station.\n");
					}
					free (musicId);
					break;

				case 'b':
					player.doQuit = 1;
					if (PianoRateTrack (&ph, curStation, curSong,
							PIANO_RATE_BAN) == PIANO_RET_OK) {
						printf ("Banned.\n");
					} else {
						printf ("Error while banning track.\n");
					}
					/* pandora does this too, I think */
					PianoDestroyPlaylist (&ph);
					curSong = NULL;
					break;

				case 'c':
					musicId = selectMusicId (&ph);
					PianoCreateStation (&ph, musicId);
					free (musicId);
					break;

				case 'd':
					printf ("Really delete \"%s\"? [yn]\n",
							curStation->name);
					read (fileno (stdin), &yesnoBuf, sizeof (yesnoBuf));
					if (yesnoBuf == 'y') {
						if (PianoDeleteStation (&ph, curStation) ==
								PIANO_RET_OK) {
							player.doQuit = 1;
							printf ("Deleted.\n");
							PianoDestroyPlaylist (&ph);
							curSong = NULL;
							curStation = selectStation (&ph);
						} else {
							printf ("Error while deleting station.\n");
						}
					}
					break;

				case 'l':
					if (curSong->rating == PIANO_RATE_LOVE) {
						printf ("Already loved. No need to do this twice.\n");
						break;
					}
					if (PianoRateTrack (&ph, curStation, curSong,
							PIANO_RATE_LOVE) == PIANO_RET_OK) {
						printf ("Loved.\n");
					} else {
						printf ("Error while loving track.\n");
					}
					break;

				case 'n':
					player.doQuit = 1;
					break;

				case 'q':
					doQuit = 1;
					player.doQuit = 1;
					break;

				case 'r':
					lineBuf = readline ("New name?\n");
					if (lineBuf != NULL && strlen (lineBuf) > 0) {
						if (PianoRenameStation (&ph, curStation, lineBuf) ==
								PIANO_RET_OK) {
							printf ("Renamed.\n");
						} else {
							printf ("Error while renaming station.\n");
						}
					}
					if (lineBuf != NULL) {
						free (lineBuf);
					}
					break;

				case 's':
					player.doQuit = 1;
					PianoDestroyPlaylist (&ph);
					curSong = NULL;
					curStation = selectStation (&ph);
					if (curStation != NULL) {
						printf ("changed station to %s\n",
								curStation->name);
					}
					break;
			}
		}
	}

	if (player.url != NULL) {
		free (player.url);
		pthread_join (playerThread, NULL);
	}
	/* destroy everything (including the world...) */
	PianoDestroy (&ph);
	curl_global_cleanup ();
	ao_shutdown();
	xmlCleanupParser ();
	BarSettingsDestroy (&bsettings);

	return 0;
}
