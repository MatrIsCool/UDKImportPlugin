#include "UDKImportPluginPrivatePCH.h"
#include "Editor/UnrealEd/Public/BSPOps.h"
#include "T3DLevelParser.h"
#include "T3DMaterialParser.h"

T3DLevelParser::T3DLevelParser(const FString &UdkPath, const FString &TmpPath) : T3DParser(UdkPath, TmpPath)
{
	this->World = NULL;
}

template<class T>
T * T3DLevelParser::SpawnActor()
{
	if (World == NULL)
	{
		FLevelEditorModule & LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		World = LevelEditorModule.GetFirstLevelEditor().Get()->GetWorld();
	}
	ensure(World != NULL);

	return World->SpawnActor<T>();
}

void T3DLevelParser::ImportLevel(const FString &Level)
{
	GWarn->BeginSlowTask(LOCTEXT("StatusBegin", "Importing requested level"), true, false);
	StatusNumerator = 0;
	StatusDenominator = 7;
	
	GWarn->StatusUpdate(++StatusNumerator, StatusDenominator, LOCTEXT("ExportUDKLevelT3D", "Exporting UDK Level informations"));
	{
		const FString CommandLine = FString::Printf(TEXT("batchexport %s Level T3D %s"), *Level, *TmpPath);
		if (0 && RunUDK(CommandLine) != 0)
			return;
	}

	GWarn->StatusUpdate(++StatusNumerator, StatusDenominator, LOCTEXT("LoadUDKLevelT3D", "Loading UDK Level informations"));
	{
		FString UdkLevelT3D;
		if (!FFileHelper::LoadFileToString(UdkLevelT3D, *(TmpPath / TEXT("PersistentLevel.T3D"))))
			return;

		ResetParser(UdkLevelT3D);
		Package = Level;
	}

	GWarn->StatusUpdate(++StatusNumerator, StatusDenominator, LOCTEXT("ParsingUDKLevelT3D", "Parsing UDK Level informations"));
	ImportLevel();

	ResolveRequirements();
	GWarn->EndSlowTask();
}

void T3DLevelParser::ResolveRequirements()
{
	bool Continue = true;
	TArray<FString> StaticMeshFiles;
	FAssetToolsModule& AssetToolsModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools");
	FString Url, Type, PackageName, Name;
	
	GWarn->StatusUpdate(++StatusNumerator, StatusDenominator, LOCTEXT("ExportStaticMeshMaterials", "Exporting StaticMesh material references"));
	ResolveMeshesRequirements();

	GWarn->StatusUpdate(++StatusNumerator, StatusDenominator, LOCTEXT("ExportUDKAssets", "Exporting UDK Assets"));
	ExportUDKAssets();
	
	GWarn->StatusUpdate(++StatusNumerator, StatusDenominator, LOCTEXT("ImportStaticMesh", "Importing Meshes"));
	StaticMeshFiles.Add(TmpPath / TEXT("Meshes"));
	AssetToolsModule.Get().ImportAssets(StaticMeshFiles, TEXT("/Game/UDK"));
	
	GWarn->StatusUpdate(++StatusNumerator, StatusDenominator, LOCTEXT("ResolvingLinks", "Updating actors assets"));
	for (auto Iter = Requirements.CreateConstIterator(); Iter && Continue; ++Iter)
	{
		Url = Iter.Key();
		ParseRessourceUrl(Url, Type, PackageName, Name);

		if (Type == TEXT("StaticMesh"))
		{
			FString ObjectPath = FString::Printf(TEXT("/Game/UDK/Meshes/%s/%s.%s"), *PackageName, *Name, *Name);
			FixRequirement(Url, FindObject<UStaticMesh>(NULL, *ObjectPath));
		}
	}
}

void T3DLevelParser::ResolveMeshesRequirements()
{
	FString Url, Type, PackageName, Name, StaticMeshUrl, MaterialUrl;
	int32 MaterialIdx;
	int32 StaticMeshesParamsCount = 0;
	FString StaticMeshesParams = TEXT("run UDKPluginExport.ExportStaticMeshMaterials");
	FString ExportStaticMeshMaterialsOutput;
	for (auto Iter = Requirements.CreateConstIterator(); Iter; ++Iter)
	{
		Url = Iter.Key();
		ParseRessourceUrl(Url, Type, PackageName, Name);

		if (Type == TEXT("StaticMesh"))
		{
			StaticMeshesParams += TEXT(" ") + Url;
			++StaticMeshesParamsCount;
			if (StaticMeshesParamsCount >= 50)
			{
				if (RunUDK(StaticMeshesParams, ExportStaticMeshMaterialsOutput) == 0)
				{
					ResetParser(ExportStaticMeshMaterialsOutput);
					while (NextLine())
					{
						if (Line.StartsWith(TEXT("ScriptLog: ")))
						{
							int32 StaticMeshUrlEndIndex = Line.Find(TEXT(" "), ESearchCase::CaseSensitive, ESearchDir::FromStart, 11);
							int32 MaterialIdxEndIndex = Line.Find(TEXT(" "), ESearchCase::CaseSensitive, ESearchDir::FromStart, StaticMeshUrlEndIndex + 1);
							if (StaticMeshUrlEndIndex != -1 && MaterialIdxEndIndex != -1)
							{
								StaticMeshUrl = Line.Mid(11, StaticMeshUrlEndIndex - 11);
								MaterialIdx = FCString::Atoi(*Line.Mid(StaticMeshUrlEndIndex + 1, MaterialIdxEndIndex - StaticMeshUrlEndIndex - 1));
								MaterialUrl = Line.Mid(MaterialIdxEndIndex + 1);
								AddRequirement(MaterialUrl, UObjectDelegate::CreateRaw(this, &T3DLevelParser::SetStaticMeshMaterial, StaticMeshUrl, MaterialIdx));
							}
						}
					}
				}
				StaticMeshesParamsCount = 0;
				StaticMeshesParams = TEXT("run UDKPluginExport.ExportStaticMeshMaterials");
			}
		}
	}
}

void T3DLevelParser::ExportUDKAssets()
{
	FString Url, Type, PackageName, Name;
	FString ExportFolder, ImportFolder, FileName;
	TSet<FString> ExportedOBJStaticMeshPackages, ExportedMaterialPackages;
	IFileManager & FileManager = IFileManager::Get();

	for (auto Iter = Requirements.CreateConstIterator(); Iter; ++Iter)
	{
		Url = Iter.Key();
		ParseRessourceUrl(Url, Type, PackageName, Name);

		if (Type == TEXT("StaticMesh"))
		{
			ExportFolder = TmpPath / TEXT("ExportedMeshes") / PackageName;
			ImportFolder = TmpPath / TEXT("Meshes") / PackageName;
			FileName = Name + TEXT(".OBJ");

			if (!ExportedOBJStaticMeshPackages.Contains(PackageName))
			{
				if (FileManager.DirectoryExists(*ExportFolder)
					|| RunUDK(FString::Printf(TEXT("batchexport %s StaticMesh OBJ %s"), *PackageName, *ExportFolder)) == 0)
				{
					ExportedOBJStaticMeshPackages.Add(PackageName);
				}
			}

			if (FileManager.FileSize(*(ExportFolder / FileName)) > 0)
			{
				if (FileManager.MakeDirectory(*ImportFolder, true))
				{
					if (FileManager.FileSize(*(ImportFolder / Name + TEXT(".FBX"))) == INDEX_NONE)
					{
						ConvertOBJToFBX(ExportFolder / FileName, ImportFolder / Name + TEXT(".FBX"));
					}
				}
			}
		}
		else if (TEXT("Material"))
		{
			ExportFolder = TmpPath / TEXT("ExportedMaterials") / PackageName;
			FileName = Name + TEXT(".T3D");

			if (!ExportedMaterialPackages.Contains(PackageName))
			{
				if (FileManager.DirectoryExists(*ExportFolder)
					|| RunUDK(FString::Printf(TEXT("batchexport %s Material T3D %s"), *PackageName, *ExportFolder)) == 0)
				{
					ExportedMaterialPackages.Add(PackageName);
				}
			}

			T3DMaterialParser Material(this);
			Material.ImportMaterialT3DFile(ExportFolder / FileName, PackageName);
		}
	}
}

void T3DLevelParser::ImportLevel()
{
	FString Class;

	ensure(NextLine());
	ensure(Line.Equals(TEXT("Begin Object Class=Level Name=PersistentLevel")));

	while (NextLine() && !IsEndObject())
	{
		if (IsBeginObject(Class))
		{
			UObject * Object = 0;
			if (Class.Equals(TEXT("StaticMeshActor")))
				ImportStaticMeshActor();
			else if (Class.Equals(TEXT("Brush")))
				ImportBrush();
			else if (Class.Equals(TEXT("PointLight")))
				ImportPointLight();
			else if (Class.Equals(TEXT("SpotLight")))
				ImportSpotLight();
			else
				JumpToEnd();
		}
	}
}

void T3DLevelParser::ImportBrush()
{
	FString Value, Class, Name;
	ABrush * Brush = SpawnActor<ABrush>();
	Brush->BrushType = Brush_Add;
	UModel* Model = new(Brush, NAME_None, RF_Transactional)UModel(FPostConstructInitializeProperties(), Brush, 1);

	while (NextLine() && !IsEndObject())
	{
		if (Line.StartsWith(TEXT("Begin Brush ")))
		{
			while (NextLine() && !Line.StartsWith(TEXT("End Brush")))
			{
				if (Line.StartsWith(TEXT("Begin PolyList")))
				{
					ImportPolyList(Model->Polys);
				}
			}
		}
		else if (GetProperty(TEXT("CsgOper="), Value))
		{
			if (Value.Equals(TEXT("CSG_Subtract")))
			{
				Brush->BrushType = Brush_Subtract;
			}
		}
		else if (IsActorLocation(Brush))
		{
			continue;
		}
		else if (Line.StartsWith(TEXT("Begin "), ESearchCase::CaseSensitive))
		{
			JumpToEnd();
		}
	}
	
	Model->Modify();
	if (!Model->Linked)
	{
		Model->Linked = 1;
		for (int32 i = 0; i<Model->Polys->Element.Num(); i++)
		{
			Model->Polys->Element[i].iLink = i;
		}

		for (int32 i = 0; i<Model->Polys->Element.Num(); i++)
		{
			FPoly* EdPoly = &Model->Polys->Element[i];
			if (EdPoly->iLink == i)
			{
				for (int32 j = i + 1; j<Model->Polys->Element.Num(); j++)
				{
					FPoly* OtherPoly = &Model->Polys->Element[j];
					if
						(OtherPoly->iLink == j
						&&	OtherPoly->Material == EdPoly->Material
						&&	OtherPoly->TextureU == EdPoly->TextureU
						&&	OtherPoly->TextureV == EdPoly->TextureV
						&&	OtherPoly->PolyFlags == EdPoly->PolyFlags
						&& (OtherPoly->Normal | EdPoly->Normal)>0.9999)
					{
						float Dist = FVector::PointPlaneDist(OtherPoly->Vertices[0], EdPoly->Vertices[0], EdPoly->Normal);
						if (Dist>-0.001 && Dist<0.001)
						{
							OtherPoly->iLink = i;
						}
					}
				}
			}
		}
	}

	// Build bounds.
	Model->BuildBound();

	Brush->BrushComponent->Brush = Brush->Brush;

	// Let the actor deal with having been imported, if desired.
	Brush->PostEditImport();
	// Notify actor its properties have changed.
	Brush->PostEditChange();
	Brush->SetFlags(RF_Standalone | RF_Public);

}

void T3DLevelParser::ImportPolyList(UPolys * Polys)
{
	FString Texture;
	while (NextLine() && !Line.StartsWith(TEXT("End PolyList")))
	{
		if (Line.StartsWith(TEXT("Begin Polygon ")))
		{
			bool GotBase = false;
			FPoly Poly;
			if (GetOneValueAfter(TEXT(" Texture="), Texture))
			{
				AddRequirement(FString::Printf(TEXT("Material'%s'"), *Texture), UObjectDelegate::CreateRaw(this, &T3DLevelParser::SetPolygonTexture, Polys, Polys->Element.Num()));
			}
			FParse::Value(*Line, TEXT("LINK="), Poly.iLink);
			Poly.PolyFlags &= ~PF_NoImport;

			while (NextLine() && !Line.StartsWith(TEXT("End Polygon")))
			{
				const TCHAR* Str = *Line;
				if (FParse::Command(&Str, TEXT("ORIGIN")))
				{
					GotBase = true;
					ParseFVector(Str, Poly.Base);
				}
				else if (FParse::Command(&Str, TEXT("VERTEX")))
				{
					FVector TempVertex;
					ParseFVector(Str, TempVertex);
					new(Poly.Vertices) FVector(TempVertex);
				}
				else if (FParse::Command(&Str, TEXT("TEXTUREU")))
				{
					ParseFVector(Str, Poly.TextureU);
				}
				else if (FParse::Command(&Str, TEXT("TEXTUREV")))
				{
					ParseFVector(Str, Poly.TextureV);
				}
				else if (FParse::Command(&Str, TEXT("NORMAL")))
				{
					ParseFVector(Str, Poly.Normal);
				}
			}
			if (!GotBase)
				Poly.Base = Poly.Vertices[0];
			if (Poly.Finalize(NULL, 1) == 0)
				new(Polys->Element)FPoly(Poly);
		}
	}
}

void T3DLevelParser::ImportPointLight()
{
	FString Value, Class;
	APointLight* PointLight = SpawnActor<APointLight>();

	while (NextLine() && !IsEndObject())
	{
		if (IsBeginObject(Class))
		{
			if (Class.Equals(TEXT("SpotLightComponent")))
			{
				while (NextLine() && IgnoreSubs() && !IsEndObject())
				{
					if (GetProperty(TEXT("Radius="), Value))
					{
						PointLight->PointLightComponent->AttenuationRadius = FCString::Atof(*Value);
					}
					else if (GetProperty(TEXT("Brightness="), Value))
					{
						PointLight->PointLightComponent->Intensity = FCString::Atof(*Value) * IntensityMultiplier;
					}
					else if (GetProperty(TEXT("LightColor="), Value))
					{
						FColor Color;
						Color.InitFromString(Value);
						PointLight->PointLightComponent->LightColor = Color;
					}
				}
			}
			else
			{
				JumpToEnd();
			}
		}
		else if (IsActorLocation(PointLight) || IsActorRotation(PointLight))
		{
			continue;
		}
	}
}

void T3DLevelParser::ImportSpotLight()
{
	FVector DrawScale3D(1.0,1.0,1.0);
	FRotator Rotator(0.0, 0.0, 0.0);
	FString Value, Class, Name;
	ASpotLight* SpotLight = SpawnActor<ASpotLight>();

	while (NextLine() && !IsEndObject())
	{
		if (IsBeginObject(Class))
		{
			if (Class.Equals(TEXT("SpotLightComponent")))
			{
				while (NextLine() && IgnoreSubs() && !IsEndObject())
				{
					if (GetProperty(TEXT("Radius="), Value))
					{
						SpotLight->SpotLightComponent->AttenuationRadius = FCString::Atof(*Value);
					}
					else if (GetProperty(TEXT("InnerConeAngle="), Value))
					{
						SpotLight->SpotLightComponent->InnerConeAngle = FCString::Atof(*Value);
					}
					else if (GetProperty(TEXT("OuterConeAngle="), Value))
					{
						SpotLight->SpotLightComponent->OuterConeAngle = FCString::Atof(*Value);
					}
					else if (GetProperty(TEXT("Brightness="), Value))
					{
						SpotLight->SpotLightComponent->Intensity = FCString::Atof(*Value) * IntensityMultiplier;
					}
					else if (GetProperty(TEXT("LightColor="), Value))
					{
						FColor Color;
						Color.InitFromString(Value);
						SpotLight->SpotLightComponent->LightColor = Color;
					}
				}
			}
			else
			{
				JumpToEnd();
			}
		}
		else if (IsActorLocation(SpotLight))
		{
			continue;
		}
		else if (GetProperty(TEXT("Rotation="), Value))
		{
			ensure(ParseUDKRotation(Value, Rotator));
		}
		else if (GetProperty(TEXT("DrawScale3D="), Value))
		{
			ensure(DrawScale3D.InitFromString(Value));
		}
	}

	// Because there is people that does this in UDK...
	SpotLight->SetActorRotation((DrawScale3D.X * Rotator.Vector()).Rotation());
}

void T3DLevelParser::ImportStaticMeshActor()
{
	FString Value, Class;
	FVector PrePivot;
	bool bPrePivotFound = false;
	AStaticMeshActor * StaticMeshActor = SpawnActor<AStaticMeshActor>();
	
	while (NextLine() && !IsEndObject())
	{
		if (IsBeginObject(Class))
		{
			if (Class.Equals(TEXT("StaticMeshComponent")))
			{
				while (NextLine() && !IsEndObject())
				{
					if (GetProperty(TEXT("StaticMesh="), Value))
					{
						AddRequirement(Value, UObjectDelegate::CreateRaw(this, &T3DLevelParser::SetStaticMesh, StaticMeshActor->StaticMeshComponent.Get()));
					}
				}
			}
			else
			{
				JumpToEnd();
			}
		}
		else if (IsActorLocation(StaticMeshActor) || IsActorRotation(StaticMeshActor) || IsActorScale(StaticMeshActor))
		{
			continue;
		}
		else if (GetProperty(TEXT("PrePivot="), Value))
		{
			ensure(PrePivot.InitFromString(Value));
			bPrePivotFound = true;
		}
	}

	if (bPrePivotFound)
	{
		PrePivot = StaticMeshActor->GetActorRotation().RotateVector(PrePivot);
		StaticMeshActor->SetActorLocation(StaticMeshActor->GetActorLocation() - PrePivot);
	}
}

USoundCue * T3DLevelParser::ImportSoundCue()
{
	USoundCue * SoundCue = 0;
	FString Value;

	while (NextLine())
	{
		if (GetProperty(TEXT("SoundClass="), Value))
		{
			// TODO
		}
		else if (GetProperty(TEXT("FirstNode="), Value))
		{
			AddRequirement(Value, UObjectDelegate::CreateRaw(this, &T3DLevelParser::SetSoundCueFirstNode, SoundCue));
		}
	}

	return SoundCue;
}

void T3DLevelParser::SetPolygonTexture(UObject * Object, UPolys * Polys, int32 index)
{
	Polys->Element[index].Material = Cast<UMaterialInterface>(Object);
}

void T3DLevelParser::SetStaticMesh(UObject * Object, UStaticMeshComponent * StaticMeshComponent)
{
	UProperty* ChangedProperty = FindField<UProperty>(UStaticMeshComponent::StaticClass(), "StaticMesh");
	UStaticMesh * StaticMesh = Cast<UStaticMesh>(Object);
	StaticMeshComponent->PreEditChange(ChangedProperty);

	StaticMeshComponent->StaticMesh = StaticMesh;

	FPropertyChangedEvent PropertyChangedEvent(ChangedProperty);
	StaticMeshComponent->PostEditChangeProperty(PropertyChangedEvent);
}

void T3DLevelParser::SetSoundCueFirstNode(UObject * Object, USoundCue * SoundCue)
{
	SoundCue->FirstNode = Cast<USoundNode>(Object);
}

void T3DLevelParser::SetStaticMeshMaterial(UObject * Material, FString StaticMeshUrl, int32 MaterialIdx)
{
	UObject * Object;
	if (FindRequirement(StaticMeshUrl, Object))
	{
		UProperty* ChangedProperty = FindField<UProperty>(UStaticMesh::StaticClass(), "Materials");
		UStaticMesh * StaticMesh = Cast<UStaticMesh>(Object);
		StaticMesh->PreEditChange(ChangedProperty);

		if (MaterialIdx >= StaticMesh->Materials.Num())
			StaticMesh->Materials.SetNum(MaterialIdx + 1);
		StaticMesh->Materials[MaterialIdx] = Cast<UMaterialInterface>(Material);
		
		FPropertyChangedEvent PropertyChangedEvent(ChangedProperty);
		StaticMesh->PostEditChangeProperty(PropertyChangedEvent);
	}
}

void T3DLevelParser::SetTexture(UObject * Object, UMaterialExpressionTextureBase * MaterialExpression)
{
	UTexture * Texture = Cast<UTexture>(Object);
	MaterialExpression->Texture = Texture;
}