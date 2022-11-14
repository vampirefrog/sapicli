Add-Type -AssemblyName System.speech 
$speak = New-Object System.Speech.Synthesis.SpeechSynthesizer
$speak.GetInstalledVoices().VoiceInfo
