# https://gist.githubusercontent.com/hiepxanh/8b6ad80f6d620cd3eaaaa5c1d2c660b2/raw/5e91890071c64ffdd728a6447ee62e199a969f79/add-voice.ps

$sourcePath = 'HKLM:\software\Microsoft\Speech_OneCore\Voices\Tokens' #Where the OneCore voices live
$destinationPath = 'HKLM:\SOFTWARE\Microsoft\Speech\Voices\Tokens' #For 64-bit apps
$destinationPath2 = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\SPEECH\Voices\Tokens' #For 32-bit apps
cd $destinationPath
$listVoices = Get-ChildItem $sourcePath
foreach($voice in $listVoices)
{
$source = $voice.PSPath #Get the path of this voices key
copy -Path $source -Destination $destinationPath -Recurse
copy -Path $source -Destination $destinationPath2 -Recurse
}
