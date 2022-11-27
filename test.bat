@echo off
setlocal enableDelayedExpansion
SET TEXT="Lorem Ipsum is simply dummy text of the printing and typesetting industry. Lorem Ipsum has been the industry's standard dummy text ever since the 1500s, when an unknown printer took a galley of type and scrambled it to make a type specimen book. It has survived not only five centuries, but also the leap into electronic typesetting, remaining essentially unchanged. It was popularised in the 1960s with the release of Letraset sheets containing Lorem Ipsum passages, and more recently with desktop publishing software like Aldus PageMaker including versions of Lorem Ipsum."
FOR %%P IN (Win32 x64) DO (
	FOR %%C IN (ReleaseStatic) DO (
		FOR %%F IN (raw wav ogg+vorbis ogg+opus) DO (
			SET EXT=%%F
			IF !EXT!==ogg+vorbis SET EXT=ogg
			IF !EXT!==ogg+opus SET EXT=ogg
			FOR %%S IN (8000 11025 12000 16000 22050 24000 44100 48000) DO (
				FOR %%B IN (8 12 16 24 32) DO (
					FOR %%N IN (1 2 3) DO (
						FOR %%E IN (0 288 all) DO (
							FOR %%V IN (DAVID ZIRA) DO (
								DEL stdout.txt
								DEL stderr.txt
								SET STDOUT=
								SET STDERR=
								%%P\%%C\sapicli.exe -T %%F -s %%S -b %%B -c %%N -e %%E -v TTS_MS_EN-US_%%V_11.0 -o test\%%P_%%C_%%F_%%S_%%B_%%N_%%E_%%V.!EXT! %TEXT% > stdout.txt 2> stderr.txt
								SET L=!errorlevel!
								SET /p STDOUT=<stdout.txt
								SET /p STDERR=<stderr.txt
								echo %%P,%%C,%%F,%%S,%%B,%%N,%%E,%%V,!L!,!STDOUT!,!STDERR!
							)
						)
					)
				)
			)
		)
	)
)
