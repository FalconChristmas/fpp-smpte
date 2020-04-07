
<?
$playlists = Array();
$playlists["--none--"] = "--none--";

if ($d = opendir($settings['playlistDirectory'])) {
    while (($file = readdir($d)) !== false) {
        if (preg_match('/\.json$/', $file)) {
            $file = preg_replace('/\.json$/', '', $file);
            //array_push($playlists, $file);
            $playlists[$file] = $file;
        }
    }
    closedir($d);
}


$AlsaOutputCards = Array();
exec("sudo aplay -l | grep '^card' | sed -e 's/^card //' -e 's/.*\[\(.*\)\].*\[\(.*\)\]/\\1, \\2/'", $output, $return_val);
$AlsaOutputCards["--Select Output Device--"] = "--none--";
foreach($output as $card) {
    $AlsaOutputCards[$card] = $card;
}
unset($output);
$AlsaInputCards = Array();
$AlsaInputCards["--Select Input Device--"] = "--none--";
exec("sudo arecord -l | grep '^card' | sed -e 's/^card //' -e 's/.*\[\(.*\)\].*\[\(.*\)\]/\\1, \\2/'", $output, $return_val);
$foundOurCard = 0;
foreach($output as $card) {
    $AlsaInputCards[$card] = $card;
}
unset($output);

$OutputFormats = Array();
$OutputFormats["30fps"] = 30;
$OutputFormats["29.97fps"] = 29.97;
$OutputFormats["25fps"] = 25;
$OutputFormats["24fps"] = 24;

?>

<div id="global" class="settings">
<? if ($settings["fppMode"] == "master" || $settings["fppMode"] == "player") { ?>
<fieldset>
<legend>SMPTE Output</legend>
<p>Enable SMPTE Output: <?php PrintSettingCheckbox("EnableSMPTEOutput", "EnableSMPTEOutput", 2, 0, "1", "0", "fpp-smpte", ""); ?></p>
<p>Output Sound Device: <?php PrintSettingSelect("SMPTEOutputDevice", "SMPTEOutputDevice", 2, 0, "", $AlsaOutputCards, "fpp-smpte", ""); ?></p>
<p>Output Frame Rate: <?php PrintSettingSelect("SMPTEOutputFrameRate", "SMPTEOutputFrameRate", 2, 0, "", $OutputFormats, "fpp-smpte", ""); ?></p>
</fieldset>
<? } else if ($settings["fppMode"] == "remote") { ?>
    
<fieldset>
<legend>SMPTE Input</legend>
<p>Enable SMPTE Input: <?php PrintSettingCheckbox("EnableSMPTEInput", "EnableSMPTEInput", 2, 0, "1", "0", "fpp-smpte", ""); ?></p>
<p>Input Sound Device: <?php PrintSettingSelect("SMPTEInputDevice", "SMPTEInputDevice", 2, 0, "", $AlsaInputCards, "fpp-smpte", ""); ?></p>
<p>Input Frame Rate: <?php PrintSettingSelect("SMPTEInputFrameRate", "SMPTEInputFrameRate", 2, 0, "", $OutputFormats, "fpp-smpte", ""); ?></p>
<p>Default Playlist: <?php PrintSettingSelect("SMPTEInputPlaylist", "SMPTEInputPlaylist", 2, 0, "", $playlists, "fpp-smpte", ""); ?></p>
<p>Send FPP MultiSync Packets: <?php PrintSettingCheckbox("SMPTEResendMultisync", "SMPTEResendMultisync", 2, 0, "1", "0", "fpp-smpte", ""); ?></p>
</fieldset>
    
<? } else { ?>
    <p>SMPTE is not supported in '<?= $settings["fppMode"] ?>' mode.</p>
    
<? } ?>
</div>

<script>
PopulatePlaylists(false);
</script>
