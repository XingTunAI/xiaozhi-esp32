param(
    [Parameter(Mandatory = $true)][Alias("FfmpegPath")][string]$Ffmpeg,
    [string]$Voice = 'Microsoft Huihui Desktop',
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$destination = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../main/assets/common'))
$prompts = [ordered]@{
    vl_wifi_setup = '请配网。请连接星豚设备热点。热点密码是。'
    vl_wifi_finish = '请在浏览器打开配网页面。配网完成后，手机请切回正常网络。'
    vl_connected = '已连接网络。'
    vl_binding_code = '请登录听见，在我的设备中选择添加设备。五分钟内输入八位绑定码。绑定码是。'
    vl_binding_done = '设备绑定成功。请在网页查看服务状态。'
    vl_reset_confirm = '恢复出厂将解除绑定，并清除网络。请在二十秒内短按 K2 确认。不操作则取消。'
    vl_reset_done = '恢复出厂成功。即将重启，请重新配网和绑定。'
    vl_reset_failed = '恢复出厂未完成。请先连接网络，结束录音，再重试。原来的设置已保留。'
    vl_recording_start = '开始会议录音。'
    vl_recording_stop = '录音已停止。'
    vl_recording_pause = '录音已暂停。'
    vl_recording_resume = '继续会议录音。'
    vl_start_requested = '已请求开始录音，请稍候。'
    vl_start_failed = '暂时无法开始录音，请在网页查看设备状态。'
    vl_interrupted = '录音已停止，请在网页检查本次结果。'
}
# First-use copy is shared with the customer delivery documentation.
$copyPath = Join-Path $PSScriptRoot '../docs/voice-lab-prompt-copy.json'
$copy = Get-Content -LiteralPath $copyPath -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $copy.PSObject.Properties) { $prompts[$entry.Name] = [string]$entry.Value }
$temporary = [IO.Path]::GetTempFileName()
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
    $synth.SelectVoice($Voice)
    $synth.Rate = 0
    foreach ($entry in $prompts.GetEnumerator()) {
        if ($Only.Count -gt 0 -and $entry.Key -notin $Only) { continue }
        $synth.SetOutputToWaveFile($temporary)
        $synth.Speak($entry.Value)
        $synth.SetOutputToNull()
        $output = Join-Path $destination ($entry.Key + '.ogg')
        & $Ffmpeg -hide_banner -loglevel error -y -i $temporary -ac 1 -ar 16000 -c:a libopus -b:a 24k -frame_duration 20 $output
        if ($LASTEXITCODE -ne 0) { throw "Audio conversion failed: $($entry.Key)" }
        Write-Output ($entry.Key + ': ' + $entry.Value)
    }
} finally {
    $synth.Dispose()
    Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue
}
