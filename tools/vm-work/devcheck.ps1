Get-PnpDevice -InstanceId "USB\VID_238B*" | ForEach-Object { $id = $_.InstanceId; $d = Get-PnpDeviceProperty -InstanceId $id -KeyName "DEVPKEY_Device_DeviceDesc"; $p = Get-PnpDeviceProperty -InstanceId $id -KeyName "DEVPKEY_Device_Problem"; "{0} desc={1} problem={2}" -f $id, $d.Data, $p.Data }
Get-PnpDevice -Class Net -ErrorAction SilentlyContinue | Out-Null
