# Copyright (C) 2016 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


.class public LErrClass;

.super Ljava/lang/Object;

.method public static errMethod()J
   .registers 8
   const/4 v0, 0x0
   const/4 v3, 0x0
   aget v1, v0, v3
   move v3, v4
   move-wide/from16 v6, v2  # v3 is a single register but used as a pair here.
   return v6
.end method
