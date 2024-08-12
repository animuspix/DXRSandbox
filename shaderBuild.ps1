
$dirs = "Shaders\Compute\", "Shaders\Graphics\", "Shaders\Raytracing\"

$computeDirNdx = 0;
$graphicsDirNdx = 1;
$raytracingDirNdx = 2;

$extensions = ".cso", (".pso", ".vso"), ".rtso"

$vsoPath = ""
$psoPath = ""

for ($dirCtr = 0; $dirCtr -lt $dirs.Count; $dirCtr++)
{
	$directory = $dirs[$dirCtr]

	$buildingCompute = ($dirCtr -eq $computeDirNdx)
	$buildingGraphics = ($dirCtr -eq $graphicsDirNdx)
	$buildingRaytracing = ($dirCtr -eq $raytracingDirNdx)
	
	$files = Get-ChildItem $directory
	for ($i = 0; $i -lt $files.Count; $i++)
	{
		$fname = $files[$i].Name
		if ($fname.Contains(".hlsl") -and -not $fname.Contains(".hlsli"))
		{
			$fFullName = $files[$i].FullName
			Write-Host "Building" $fFullName

			$outDir = "Shaders\" + $files[$i].BaseName
			$outPath = $outDir + $extensions[$dirCtr]
			$success = $false

			# Useful DXC flags reference partway down this page
			# https://simoncoenen.com/blog/programming/graphics/DxcCompiling

			# Compute build
			if ($buildingCompute)
			{
				thirdparty\\dxc_2024_07_31\\bin\\x64\\dxc.exe -E main -T cs_6_5 -Fo $outPath $fFullName
				$success = $? # Weird and obscure error check, but it works!
			}

			# Graphics build
			elseif ($buildingGraphics)
			{
				$psoPath = $outDir + $extensions[$dirCtr][0]
				thirdparty\\dxc_2024_07_31\\bin\\x64\\dxc.exe -E main_ps -T ps_6_5 -Fo $psoPath $fFullName
				$success = $?

				$vsoPath = $outDir + $extensions[$dirCtr][1]
				thirdparty\\dxc_2024_07_31\\bin\\x64\\dxc.exe -E main_vs -T vs_6_5 -Fo $vsoPath $fFullName
				$success = $success -and $?
			}
			
			# DXR/RT build
			elseif ($buildingRaytracing)
			{
				# not using this compile path for now; lots of other things to verify before we get to checking DXR shader build errors ^_^'
				# thirdparty\\dxc_2022_12_16\\bin\\x64\\dxc.exe -E main -T lib_6_5 -Fo $outPath $fFullName
				# $success = $?
			}

			# Feedback
			# Error feedback should come automatically, so we just record successes here
			if ($success)
			{
				if ($buildingCompute -or $buildingRaytracing)
				{
					$fullOutPath = [System.IO.Path]::GetFullPath($outPath)
					if ($buildingCompute)
					{
						Write-Host "Successful compute build, see" $fullOutPath `r`n
					}
					else
					{
						Write-Host "Successful raytracing build, see" $fullOutPath `r`n						
					}
				}
				elseif ($buildingGraphics)
				{
					$vsoFullPath = [System.IO.Path]::GetFullPath($vsoPath)
					$psoFullPath = [System.IO.Path]::GetFullPath($psoPath)
					Write-Host "Successful graphics builds, see" $vsoFullPath "(vertex shader)," $psoFullPath "(pixel shader)" `r`n					
				}
			}	
		}
	}
}