# Monstres 3D (Granny `.gr2`) — rétro-ingénierie et rendu

Une poignée de « monstres » ne sont pas des sprites : l'**Emperium**, les trois
**gardiens de forteresse**, le **drapeau de guilde** et les **coffres au trésor**
sont des acteurs 3D rendus par `granny2.dll`. Nos fenêtres cherchaient un `.spr`
et affichaient « pas de sprite ».

## 1. Comment le client décide

🔴 **Il n'existe aucune liste d'ids à recopier.** Le marqueur est l'**extension**
rendue par `jobName.lub`, la table que `Job_GetDisplayNameOrResName`
(`0x00D5BB40`, ctx `0x015FA3C0`, sex = −1) interroge déjà :

```lua
[jobtbl.JT_CHONCHON]        = "Chocho",             -- sprite : 몬스터\Chocho.spr
[jobtbl.JT_EMPELIUM]        = "Empelium90_0.gr2",   -- modèle : model\3dmob\Empelium90_0.gr2
```

`sub_71F600` (`0x0071F600`) recolle ce nom au gabarit **`model\3dmob\%s`** — noter
le `%s` **sans** extension, elle vient du nom — puis `Res_MakeDataRootRelativePath`
ajoute `data\`. Le test est donc : *le nom finit-il par `.gr2` ?*

**85 classes** portent un `.gr2`, pour **8 modèles distincts**, dont 6 livrés :

| Modèle | Classes | Contenu |
|---|---|---|
| `TREASUREBOX_2.gr2` | 70 (coffres WoE/MVP) | 1 mesh, 22 os, anim 3,0 s |
| `Aguardian90_8.gr2` | Archer Guardian + variantes | 6 meshes, 43 os, anim 3,3 s |
| `Kguardian90_7.gr2` | Knight/Sword Guardian | 5 meshes, 35 os, anim 2,0 s |
| `Sguardian90_9.gr2` | Soldier Guardian | 4 meshes, 31 os, anim 2,0 s |
| `Empelium90_0.gr2` | 1288, `JT_DREAMMETAL`, `JT_ROC_EMPELIUM` | 1 mesh, 18 os, anim 9,9 s |
| `Guildflag90_1.gr2` | 722 | 2 meshes (dont l'emblème), 41 os, anim 5,7 s |

`dragon_5.gr2` (`JT_ZOMBIE_DRAGON`) et `Hugeling90_6.gr2` (`JT_HUGELING`)
n'existent nulle part : le client ne les affiche pas non plus.

`sub_71F600` porte aussi une table de 10 classes (`classId`, `0x011F4660` :
1288, 722, 1324, 1000×4, 1286, 1285, 1287) — elle ne sert qu'aux **poses
supplémentaires** `model\3dmob_bone\<n>_<nom>.gr2`, pas à la décision 2D/3D.

## 2. Le format : ne pas le parser

`.gr2` est propriétaire et compressé — GRF Editor ne le rend pas. **`granny2.dll`
est livrée avec le client** (importée statiquement, donc déjà en mémoire) et
suffit à tout faire, skinning compris. Le client exige
`GrannyVersionsMatch(2,1,0,5)` (`0x0071B9C1`) ; la DLL livrée rend `2.1.0.5`.

🔴 **`GrannyPNT332VertexType` et `GrannyRGBA8888PixelFormat` sont des variables
qui CONTIENNENT un pointeur.** Il faut déréférencer une fois de plus que pour un
symbole ordinaire :

```asm
mov eax, ds:GrannyPNT332VertexType   ; eax = &variable      (0x007212D0)
push dword ptr [eax]                 ; on pousse son CONTENU
```

Passer l'adresse de la variable fait crasher la DLL **dans sa propre lecture du
descripteur** — une violation d'accès qui ne ressemble pas à une erreur
d'appelant. C'est le premier mur rencontré.

### Offsets, mesurés dans le client

Aucun en-tête Granny n'est disponible ; tout vient de `C3dGrannyModelRes_Load`
(`0x0071B9B0`), `sub_7211D0` (`0x007211D0`), `sub_721420` (`0x00721420`), et a
été revérifié sur les six modèles.

| Structure | Offset | Champ |
|---|---|---|
| `granny_file_info` | +16 / +20 | TextureCount / Textures |
| | +24 / +28 | MaterialCount / Materials |
| | +32 / +36 | SkeletonCount / Skeletons |
| | +40 / +44 | VertexDataCount / VertexDatas |
| | +48 / +52 | TriTopologyCount / TriTopologies |
| | +56 / +60 | MeshCount / Meshes |
| | +64 / +68 | ModelCount / Models |
| | +72 / +76 | TrackGroupCount / TrackGroups |
| | +80 / +84 | AnimationCount / Animations |
| `granny_model` | +0 / +4 | Name / Skeleton |
| | +76 / +80 | MeshBindingCount / MeshBindings (`granny_transform` = 68 o en +8) |
| `granny_skeleton` | +0 / +4 | Name / BoneCount |
| `granny_mesh` | +0 | Name |
| | +20 / +24 | MaterialBindingCount / MaterialBindings |
| `granny_texture` | +0 / +4 | FromFileName / TextureType |
| | +8 / +12 | Width / Height |
| | +60 | ImageCount |
| `granny_animation` | +0 / +4 | Name / Duration (s) |

**Les textures sont EMBARQUÉES** dans le `.gr2` (le fichier porte jusqu'au chemin
de la machine de l'artiste : `D:\ChalesWORK_2003\0226empelium\empelium.tif`).
Aucun accès au GRF n'est nécessaire. Décodage :
`GrannyCopyTextureImage(tex, 0, 0, RGBA8888, w, h, 4*w, dst)`. Le client refuse
tout ce qui n'est pas `TextureType == 0` et `ImageCount == 1`.

La texture d'un groupe de triangles se retrouve par
`GrannyGetMaterialTextureByType(materialBinding, 2)`, puis en comparant son
**nom de fichier** (premier champ) à ceux du `file_info` — il n'y a pas d'index
direct, et c'est exactement ce que fait le client.

## 3. Le pipeline d'animation, tel que le client l'exécute

`sub_725350` (`0x00725350`) — une fois par frame :

```c
skel  = GrannyGetSourceSkeleton(instance);
bones = *(skel + 4);
GrannySetModelClock(instance, t);
GrannySampleModelAnimations(instance, 0, bones, localPose);
GrannyBuildWorldPose(skel, 0, bones, localPose, NULL, worldPose);
GrannyFreeCompletedModelControls(instance);
```

`sub_724EF0` (`0x00724EF0`) — par mesh :

```c
toBones = GrannyGetMeshBindingToBoneIndices(binding);
GrannyDeformVertices(deformer, toBones, matrices, vcount,
                     GrannyGetMeshVertices(mesh), dst);
```

🔴 **`matrices` = `GrannyGetWorldPoseComposite4x4Array`, PAS
`GrannyGetWorldPose4x4Array`.** Les deux existent, rendent un tableau de même
taille, et l'API n'en refuse aucun : avec le mauvais, le skinning « marche » de
travers et les meshes d'un même monstre **partent chacun de leur côté**. Ça
ressemble à un problème de repère et n'en est pas un. Seules les composites
annulent la pose de liaison. Symptôme exact observé : le coffre et l'Emperium
(un seul mesh) étaient corrects, les gardiens éparpillés.

**Tous** les meshes des six modèles sont *souples* : le skinning n'est pas
optionnel. Le cas rigide (`GrannyMeshIsRigid`) existe dans le client — matrice
composite de l'os `toBones[0]` appliquée aux sommets bruts — mais aucun modèle
livré ne l'emprunte.

### Vitesse de lecture

🔴 **Le client joue ces animations à vitesse DOUBLE.** `CActorSprite_RenderModel`
(`0x00C5BB10`) avance son horloge de `SecondsElapsed + SecondsElapsed` avant
`GrannySetModelClock`. À vitesse réelle, l'Emperium (9,9 s de cycle) paraît
ramer à côté du jeu — c'est le premier défaut remonté à l'essai.

### Orientation

Après `BuildWorldPose`, **Z est la verticale** (Emperium : z 0 → 23,45 ;
drapeau : z −0,84 → 41,52). Avant de projeter, le client applique une rotation
autour de **X** dont l'angle dépend du **nom du fichier** (`0x00C5CF10`) :

```c
angle = strstr(modelName, "90") ? PI/2 : PI;   // 1070141400 / 1078530008
```

Les cinq modèles « …90_… » portent donc la convention de l'artiste ; seul
`TREASUREBOX_2` prend π.

### Angle de caméra

🔴 **45° de plongée, pas 20°.** `Camera_DragControl` (`0x00C79F90`) borne
l'inclinaison de la caméra (`camera+0x44`, la caméra étant `*(CGameMode+0xD0)`)
autour de −45° à −60°. Un modèle rendu trop à plat paraît « pas comme en jeu »
sans qu'on sache dire pourquoi : l'œil le compare aux acteurs de la carte, juste
à côté. Second défaut remonté à l'essai.

## 4. Ce qui est livré

| Fichier | Rôle |
|---|---|
| `src/ui/gr2_model.{h,cc}` | Chargement, textures, pose, déformation. Aucune dépendance ImGui/DirectX. |
| `src/ui/model_raster.{h,cc}` | Rasteriseur logiciel (z-buffer, texture au plus proche voisin, SSAA 2×) → image RGBA. |
| `src/ui/mob_model.{h,cc}` | Pré-rendu de l'animation en textures d'overlay, affichage comme un sprite. |
| `tools/gr2_dump.cc` | Sonde hors client : structure, textures en BMP, contrôles de cohérence. |
| `tools/gr2_render.cc` | Rendu hors client des mêmes sources que le jeu. |

**Rendu logiciel et non DirectX** : le jeu tourne en DX7 **ou** DX9 selon le
Setup, un mob pèse 6 à 300 triangles, et écrire deux chemins graphiques pour six
modèles ne se justifie pas. **Rendu en images gardées** : une texture créée à
chaque frame — puis relâchée alors qu'une draw-list la référence encore —
corrompt le tas (`0xC0000374`) ; en passant par des images, l'affichage redevient
celui d'un sprite.

Les images sont rasterisées **à la demande**, pas toutes d'avance : le premier
tour d'animation remplit le cache au rythme d'une image par frame, les tours
suivants ne coûtent rien, et un cran de molette (qui change l'orientation et
invalide la série) ne déclenche plus une centaine de rasterisations d'un coup.
Pas visé 40 ms (~25 images/s), plafond 96 images ; au-delà le pas s'élargit au
lieu de tronquer l'animation.

### Vérification

Comme pour `.spr`/`.act`, tout a été prouvé **hors du client** avant compilation
dans le jeu :

```
"…\vcvarsall.bat" x86
cl /std:c++17 /EHsc /O2 /I src tools\gr2_render.cc src\ui\gr2_model.cc src\ui\model_raster.cc
gr2_render.exe "E:\…\data\model\3dmob\empelium90_0.gr2" sortie --dll "E:\…\granny2.dll" --images 6
```

x86 obligatoire (`granny2.dll` est 32 bits). Les six modèles sortent debout,
texturés, éclairés et animés.
