function updateStatus(){
  fetch('/api/status').then(function(r){return r.json()}).then(function(d){
    document.getElementById('device_id').innerText=d.device_id||'N/A';
    document.getElementById('config_state').innerText=(d.config_state||'N/A')+(d.config_degraded?' (降级，已保存配置保持原值)':'');
    document.getElementById('ip').innerText=d.ip||'N/A';
    document.getElementById('ssid').innerText=d.ssid||'N/A';
    document.getElementById('mode').innerText=d.mode==0?'STA':'AP';
    document.getElementById('heap').innerText=d.heap+' bytes';
    document.getElementById('uptime').innerText=d.uptime+'s';
    document.getElementById('title').textContent=d.title||'暂无内容';
    document.getElementById('error').textContent=d.last_error||'ESP_OK';
    var s=document.getElementById('status');
    if(d.status==2){s.className='status status-connected';s.innerText='Connected';}
    else if(d.status==4){s.className='status status-ap';s.innerText='AP Mode';}
    else{s.className='status status-disconnected';s.innerText='Disconnected';}
  }).catch(function(){});
}
setInterval(updateStatus,2000);
updateStatus();
