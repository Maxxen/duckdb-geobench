# Geobench

This repository is based on https://github.com/duckdb/extension-template, check it out if you want to build and ship your own DuckDB extension.
---

# Results

## Overture buildings extract, 10x scale

Original dataset 1_616_014 rows, upscaled 10x.
- Lots of small polygons and mulitpolygons.
- Average of ~7 vertices per geometry

| Benchmark | Average Time (s) | Min Time    | Max Time |
|-----------|------------------|-------------|----------|
| benchmark/bkb_area.test                  | 0.061           | 0.059      | 0.066   |
| benchmark/bkb_area_fast.test             | 0.034           | 0.033      | 0.038   |
| benchmark/wkb_area.test                  | 0.051           | 0.049      | 0.052   |
| benchmark/wkb_area_fast.test             | 0.040           | 0.039      | 0.044   |
| benchmark/bkb_extent.test                | 0.056           | 0.054      | 0.059   |
| benchmark/bkb_extent_fast.test           | 0.033           | 0.031      | 0.036   |
| benchmark/wkb_extent_fast.test           | 0.063           | 0.061      | 0.067   |
| benchmark/wkb_extent.test                | 0.080           | 0.078      | 0.082   |
| benchmark/bkb_to_wkb.test                | 0.058           | 0.057      | 0.062   |
| benchmark/bkb_to_wkb_dynamic.test        | 0.064           | 0.062      | 0.067   |
| benchmark/bkb_to_wkb_visitor.test        | 0.076           | 0.075      | 0.079   |
| benchmark/wkb_to_wkb_cast.test           | 0.023           | 0.020      | 0.037   |
| benchmark/wkb_to_wkb_copy.test           | 0.037           | 0.035      | 0.040   |
| benchmark/wkb_to_wkb_visitor.test        | 0.072           | 0.071      | 0.076   |
| benchmark/bkb_from_wkb.test              | 0.081           | 0.080      | 0.084   |
| benchmark/wkb_from_wkb.test              | 0.087           | 0.085      | 0.092   |
| benchmark/wkb_from_wkb_le.test           | 0.084           | 0.083      | 0.087   |
| benchmark/bkb_flip.test                  | 0.054           | 0.051      | 0.057   |
| benchmark/wkb_flip.test                  | 0.053           | 0.051      | 0.056   |
| benchmark/bkb_flip_x.test                | 0.180           | 0.175      | 0.190   |
| benchmark/wkb_flip_x.test                | 0.178           | 0.176      | 0.183   |


## OSM roads geofabrik extract, 100x scale

- Converted from Niedersachsen (mit bremen) shapefile extract to parquet
- Mostly linestrings and multilinestrings
- Average of ~6 vertices per geometry
- Original dataset 1_570_627 rows, upscaled 100x to 157_062_700 rows

| Benchmark | Average Time (s) | Min Time    | Max Time |
|-----------|------------------|-------------|----------|
| benchmark/bkb_area.test                  | 0.313           | 0.304      | 0.322   |
| benchmark/bkb_area_fast.test             | 0.223           | 0.217      | 0.228   |
| benchmark/wkb_area.test                  | 0.309           | 0.295      | 0.322   |
| benchmark/wkb_area_fast.test             | 0.261           | 0.255      | 0.270   |
| benchmark/bkb_extent.test                | 0.483           | 0.475      | 0.489   |
| benchmark/bkb_extent_fast.test           | 0.256           | 0.247      | 0.266   |
| benchmark/wkb_extent_fast.test           | 0.504           | 0.495      | 0.516   |
| benchmark/wkb_extent.test                | 0.663           | 0.656      | 0.671   |
| benchmark/bkb_to_wkb.test                | 0.524           | 0.510      | 0.540   |
| benchmark/bkb_to_wkb_dynamic.test        | 0.601           | 0.594      | 0.609   |
| benchmark/bkb_to_wkb_visitor.test        | 0.718           | 0.713      | 0.726   |
| benchmark/wkb_to_wkb_cast.test           | 0.164           | 0.160      | 0.170   |
| benchmark/wkb_to_wkb_copy.test           | 0.314           | 0.302      | 0.323   |
| benchmark/wkb_to_wkb_visitor.test        | 0.684           | 0.674      | 0.696   |
| benchmark/bkb_from_wkb.test              | 0.605           | 0.598      | 0.613   |
| benchmark/wkb_from_wkb.test              | 0.761           | 0.758      | 0.764   |
| benchmark/wkb_from_wkb_le.test           | 0.767           | 0.763      | 0.770   |
| benchmark/bkb_flip.test                  | 0.414           | 0.409      | 0.427   |
| benchmark/wkb_flip.test                  | 0.463           | 0.451      | 0.473   |
| benchmark/bkb_flip_x.test                | 1.398           | 1.389      | 1.415   |
| benchmark/wkb_flip_x.test                | 1.616           | 1.597      | 1.638   |

## Citibike Bike Trips NYC dataset

- 58_033_724 rows of `POINT` geometries

| Benchmark | Average Time (s) | Min Time    | Max Time |
|-----------|------------------|-------------|----------|
| benchmark/bkb_area.test                  | 0.078           | 0.077      | 0.079   |
| benchmark/bkb_area_fast.test             | 0.046           | 0.045      | 0.047   |
| benchmark/wkb_area.test                  | 0.056           | 0.056      | 0.057   |
| benchmark/wkb_area_fast.test             | 0.056           | 0.055      | 0.057   |
| benchmark/bkb_extent.test                | 0.104           | 0.103      | 0.105   |
| benchmark/bkb_extent_fast.test           | 0.040           | 0.039      | 0.041   |
| benchmark/wkb_extent_fast.test           | 0.071           | 0.070      | 0.072   |
| benchmark/wkb_extent.test                | 0.096           | 0.094      | 0.097   |
| benchmark/bkb_to_wkb.test                | 0.115           | 0.114      | 0.116   |
| benchmark/bkb_to_wkb_dynamic.test        | 0.146           | 0.145      | 0.147   |
| benchmark/bkb_to_wkb_visitor.test        | 0.168           | 0.167      | 0.169   |
| benchmark/wkb_to_wkb_cast.test           | 0.018           | 0.014      | 0.019   |
| benchmark/wkb_to_wkb_copy.test           | 0.069           | 0.068      | 0.070   |
| benchmark/wkb_to_wkb_visitor.test        | 0.156           | 0.154      | 0.160   |
| benchmark/bkb_from_wkb.test              | 0.131           | 0.129      | 0.132   |
| benchmark/wkb_from_wkb.test              | 0.138           | 0.136      | 0.138   |
| benchmark/wkb_from_wkb_le.test           | 0.138           | 0.137      | 0.140   |
| benchmark/bkb_flip.test                  | 0.061           | 0.060      | 0.061   |
| benchmark/wkb_flip.test                  | 0.065           | 0.064      | 0.066   |
| benchmark/bkb_flip_x.test                | 0.216           | 0.215      | 0.217   |
| benchmark/wkb_flip_x.test                | 0.235           | 0.234      | 0.237   |

# Overture Divsion Area, 10x scale

- MULTIPOLYGON geometries
- Average of ~342 vertices per geometry
- Original dataset 1_027_671 rows, upscaled 2x to 2_055_342 rows

## Benchmark Results

| Benchmark | Average Time (s) | Min Time    | Max Time |
|-----------|------------------|-------------|----------|
| benchmark/bkb_area.test                  | 0.137           | 0.131      | 0.146   |
| benchmark/bkb_area_fast.test             | 0.149           | 0.128      | 0.209   |
| benchmark/wkb_area.test                  | 0.212           | 0.149      | 0.413   |
| benchmark/wkb_area_fast.test             | 0.172           | 0.127      | 0.330   |
| benchmark/bkb_extent.test                | 0.380           | 0.333      | 0.535   |
| benchmark/bkb_extent_fast.test           | 0.247           | 0.149      | 0.606   |
| benchmark/wkb_extent_fast.test           | 0.462           | 0.419      | 0.564   |
| benchmark/wkb_extent.test                | 0.454           | 0.420      | 0.561   |
| benchmark/bkb_to_wkb.test                | 0.372           | 0.243      | 0.867   |
| benchmark/bkb_to_wkb_dynamic.test        | 0.384           | 0.263      | 0.755   |
| benchmark/bkb_to_wkb_visitor.test        | 0.348           | 0.266      | 0.644   |
| benchmark/wkb_to_wkb_cast.test           | 0.038           | 0.020      | 0.097   |
| benchmark/wkb_to_wkb_copy.test           | 0.333           | 0.242      | 0.680   |
| benchmark/wkb_to_wkb_visitor.test        | 0.382           | 0.265      | 0.547   |
| benchmark/bkb_from_wkb.test              | 0.337           | 0.272      | 0.487   |
| benchmark/wkb_from_wkb.test              | 0.436           | 0.382      | 0.609   |
| benchmark/wkb_from_wkb_le.test           | 0.432           | 0.380      | 0.582   |
| benchmark/bkb_flip.test                  | 0.434           | 0.341      | 0.790   |
| benchmark/wkb_flip.test                  | 0.428           | 0.336      | 0.777   |
| benchmark/bkb_flip_x.test                | 1.342           | 1.218      | 1.738   |
| benchmark/wkb_flip_x.test                | 1.342           | 1.249      | 1.682   |



