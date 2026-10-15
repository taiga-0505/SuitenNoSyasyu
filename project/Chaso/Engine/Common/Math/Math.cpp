#include "Math.h"
#include <assert.h>
#include <cmath>
#include <math.h>

using namespace RC;

//==================================
// Vector3 関連関数
//==================================

Vector3 Add(const Vector3 &v1, const Vector3 &v2) {
  Vector3 result;

  result.x = v1.x + v2.x;
  result.y = v1.y + v2.y;
  result.z = v1.z + v2.z;

  return result;
}

Vector3 Subtract(const Vector3 &v1, const Vector3 &v2) {
  Vector3 result;

  result.x = v1.x - v2.x;
  result.y = v1.y - v2.y;
  result.z = v1.z - v2.z;

  return result;
}

Vector3 Multiply(const Vector3 &v1, float scalar) {
  Vector3 result;

  result.x = v1.x * scalar;
  result.y = v1.y * scalar;
  result.z = v1.z * scalar;

  return result;
}

float Dot(const Vector3 &v1, const Vector3 &v2) {
  Vector3 result;

  result.x = v1.x * v2.x;
  result.y = v1.y * v2.y;
  result.z = v1.z * v2.z;

  return result.x + result.y + result.z;
}

float Length(const Vector3 &v) {
  float result;

  result = v.x * v.x + v.y * v.y + v.z * v.z;

  result = sqrtf(result);

  return result;
}

Vector3 Normalize(const Vector3 &v) {
  Vector3 result;
  float length = Length(v);
  if (length != 0) {
    result.x = v.x / length;
    result.y = v.y / length;
    result.z = v.z / length;
  } else {
    result.x = 0;
    result.y = 0;
    result.z = 0;
  }
  return result;
}

Vector3 Vector3Transform(const Vector3 &vector, const Matrix4x4 &matrix) {
  Vector3 result;

  result.x = vector.x * matrix.m[0][0] + vector.y * matrix.m[1][0] +
             vector.z * matrix.m[2][0] + 1.0f * matrix.m[3][0];

  result.y = vector.x * matrix.m[0][1] + vector.y * matrix.m[1][1] +
             vector.z * matrix.m[2][1] + 1.0f * matrix.m[3][1];

  result.z = vector.x * matrix.m[0][2] + vector.y * matrix.m[1][2] +
             vector.z * matrix.m[2][2] + 1.0f * matrix.m[3][2];

  float w = vector.x * matrix.m[0][3] + vector.y * matrix.m[1][3] +
            vector.z * matrix.m[2][3] + 1.0f * matrix.m[3][3];

  if (w == 0) {
    w = 1.0f;
  }

  result.x /= w;
  result.y /= w;
  result.z /= w;

  return result;
}

Vector3 project(const Vector3 &v1, const Vector3 &v2) {
  float dot = Dot(v1, v2);
  float lengthSq = Dot(v2, v2); // v2 の長さの 2 乗

  if (lengthSq == 0) {
    return {0, 0, 0};
  }

  float scalar = dot / lengthSq;
  Vector3 result = Multiply(v2, scalar);

  return result;
}

Vector3 closestPoint(const Vector3 &point, const Segment &segment) {

  // 線分の始点
  const Vector3 &a = segment.origin;
  // 線分の終点
  Vector3 b = Add(segment.origin, segment.diff);

  // ab ベクトル
  Vector3 ab = Subtract(b, a);
  // ap ベクトル
  Vector3 ap = Subtract(point, a);

  float abLenSq = Dot(ab, ab);
  if (abLenSq == 0.0f) {
    // 線分の長さが 0 の場合、始点を返す
    return a;
  }

  // 最近接点のパラメータ t を計算（0 <= t <= 1 にクランプ）
  float t = Dot(ap, ab) / abLenSq;
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;

  // 最近接点を計算
  Vector3 result = Add(a, Multiply(ab, t));
  return result;

}

Vector3 Cross(const Vector3 &v1, const Vector3 &v2) {
  Vector3 result;

  result.x = v1.y * v2.z - v1.z * v2.y;
  result.y = v1.z * v2.x - v1.x * v2.z;
  result.z = v1.x * v2.y - v1.y * v2.x;

  return result;
}

//==================================
// Matrix4x4 関連関数
//==================================

Matrix4x4 Add(const Matrix4x4 &m1, const Matrix4x4 &m2) {
  Matrix4x4 result;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      result.m[i][j] = m1.m[i][j] + m2.m[i][j];
    }
  }
  return result;
}

Matrix4x4 Subtract(const Matrix4x4 &m1, const Matrix4x4 &m2) {
  Matrix4x4 result;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      result.m[i][j] = m1.m[i][j] - m2.m[i][j];
    }
  }
  return result;
}

Matrix4x4 Multiply(const Matrix4x4 &m1, const Matrix4x4 &m2) {
  Matrix4x4 result;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      result.m[i][j] = 0;
      for (int k = 0; k < 4; ++k) {
        result.m[i][j] += m1.m[i][k] * m2.m[k][j];
      }
    }
  }
  return result;
}

Matrix4x4 Inverse(const Matrix4x4 &m) {
  // 行列式 |A|
  float det = 0;

  Matrix4x4 result;

  // 行列式の計算
  det = (m.m[0][0] * m.m[1][1] * m.m[2][2] * m.m[3][3]) +
        (m.m[0][0] * m.m[1][2] * m.m[2][3] * m.m[3][1]) +
        (m.m[0][0] * m.m[1][3] * m.m[2][1] * m.m[3][2])

        - (m.m[0][0] * m.m[1][3] * m.m[2][2] * m.m[3][1]) -
        (m.m[0][0] * m.m[1][2] * m.m[2][1] * m.m[3][3]) -
        (m.m[0][0] * m.m[1][1] * m.m[2][3] * m.m[3][2])

        - (m.m[0][1] * m.m[1][0] * m.m[2][2] * m.m[3][3]) -
        (m.m[0][2] * m.m[1][0] * m.m[2][3] * m.m[3][1]) -
        (m.m[0][3] * m.m[1][0] * m.m[2][1] * m.m[3][2])

        + (m.m[0][3] * m.m[1][0] * m.m[2][2] * m.m[3][1]) +
        (m.m[0][2] * m.m[1][0] * m.m[2][1] * m.m[3][3]) +
        (m.m[0][1] * m.m[1][0] * m.m[2][3] * m.m[3][2])

        + (m.m[0][1] * m.m[1][2] * m.m[2][0] * m.m[3][3]) +
        (m.m[0][2] * m.m[1][3] * m.m[2][0] * m.m[3][1]) +
        (m.m[0][3] * m.m[1][1] * m.m[2][0] * m.m[3][2])

        - (m.m[0][3] * m.m[1][2] * m.m[2][0] * m.m[3][1]) -
        (m.m[0][2] * m.m[1][1] * m.m[2][0] * m.m[3][3]) -
        (m.m[0][1] * m.m[1][3] * m.m[2][0] * m.m[3][2])

        - (m.m[0][1] * m.m[1][2] * m.m[2][3] * m.m[3][0]) -
        (m.m[0][2] * m.m[1][3] * m.m[2][1] * m.m[3][0]) -
        (m.m[0][3] * m.m[1][1] * m.m[2][2] * m.m[3][0])

        + (m.m[0][3] * m.m[1][2] * m.m[2][1] * m.m[3][0]) +
        (m.m[0][2] * m.m[1][1] * m.m[2][3] * m.m[3][0]) +
        (m.m[0][1] * m.m[1][3] * m.m[2][2] * m.m[3][0]);

  result.m[0][0] = 1 / det *
                   ((m.m[1][1] * m.m[2][2] * m.m[3][3]) +
                    (m.m[1][2] * m.m[2][3] * m.m[3][1]) +
                    (m.m[1][3] * m.m[2][1] * m.m[3][2]) -
                    (m.m[1][3] * m.m[2][2] * m.m[3][1]) -
                    (m.m[1][2] * m.m[2][1] * m.m[3][3]) -
                    (m.m[1][1] * m.m[2][3] * m.m[3][2]));

  result.m[0][1] = 1 / det *
                   ((-1 * m.m[0][1] * m.m[2][2] * m.m[3][3]) -
                    (m.m[0][2] * m.m[2][3] * m.m[3][1]) -
                    (m.m[0][3] * m.m[2][1] * m.m[3][2]) +
                    (m.m[0][3] * m.m[2][2] * m.m[3][1]) +
                    (m.m[0][2] * m.m[2][1] * m.m[3][3]) +
                    (m.m[0][1] * m.m[2][3] * m.m[3][2]));

  result.m[0][2] = 1 / det *
                   ((m.m[0][1] * m.m[1][2] * m.m[3][3]) +
                    (m.m[0][2] * m.m[1][3] * m.m[3][1]) +
                    (m.m[0][3] * m.m[1][1] * m.m[3][2]) -
                    (m.m[0][3] * m.m[1][2] * m.m[3][1]) -
                    (m.m[0][2] * m.m[1][1] * m.m[3][3]) -
                    (m.m[0][1] * m.m[1][3] * m.m[3][2]));

  result.m[0][3] = 1 / det *
                   ((-1 * m.m[0][1] * m.m[1][2] * m.m[2][3]) -
                    (m.m[0][2] * m.m[1][3] * m.m[2][1]) -
                    (m.m[0][3] * m.m[1][1] * m.m[2][2]) +
                    (m.m[0][3] * m.m[1][2] * m.m[2][1]) +
                    (m.m[0][2] * m.m[1][1] * m.m[2][3]) +
                    (m.m[0][1] * m.m[1][3] * m.m[2][2]));

  result.m[1][0] = 1 / det *
                   ((-1 * m.m[1][0] * m.m[2][2] * m.m[3][3]) -
                    (m.m[1][2] * m.m[2][3] * m.m[3][0]) -
                    (m.m[1][3] * m.m[2][0] * m.m[3][2]) +
                    (m.m[1][3] * m.m[2][2] * m.m[3][0]) +
                    (m.m[1][2] * m.m[2][0] * m.m[3][3]) +
                    (m.m[1][0] * m.m[2][3] * m.m[3][2]));

  result.m[1][1] = 1 / det *
                   ((m.m[0][0] * m.m[2][2] * m.m[3][3]) +
                    (m.m[0][2] * m.m[2][3] * m.m[3][0]) +
                    (m.m[0][3] * m.m[2][0] * m.m[3][2]) -
                    (m.m[0][3] * m.m[2][2] * m.m[3][0]) -
                    (m.m[0][2] * m.m[2][0] * m.m[3][3]) -
                    (m.m[0][0] * m.m[2][3] * m.m[3][2]));

  result.m[1][2] = 1 / det *
                   ((-1 * m.m[0][0] * m.m[1][2] * m.m[3][3]) -
                    (m.m[0][2] * m.m[1][3] * m.m[3][0]) -
                    (m.m[0][3] * m.m[1][0] * m.m[3][2]) +
                    (m.m[0][3] * m.m[1][2] * m.m[3][0]) +
                    (m.m[0][2] * m.m[1][0] * m.m[3][3]) +
                    (m.m[0][0] * m.m[1][3] * m.m[3][2]));

  result.m[1][3] = 1 / det *
                   ((m.m[0][0] * m.m[1][2] * m.m[2][3]) +
                    (m.m[0][2] * m.m[1][3] * m.m[2][0]) +
                    (m.m[0][3] * m.m[1][0] * m.m[2][2]) -
                    (m.m[0][3] * m.m[1][2] * m.m[2][0]) -
                    (m.m[0][2] * m.m[1][0] * m.m[2][3]) -
                    (m.m[0][0] * m.m[1][3] * m.m[2][2]));

  result.m[2][0] = 1 / det *
                   ((m.m[1][0] * m.m[2][1] * m.m[3][3]) +
                    (m.m[1][1] * m.m[2][3] * m.m[3][0]) +
                    (m.m[1][3] * m.m[2][0] * m.m[3][1]) -
                    (m.m[1][3] * m.m[2][1] * m.m[3][0]) -
                    (m.m[1][1] * m.m[2][0] * m.m[3][3]) -
                    (m.m[1][0] * m.m[2][3] * m.m[3][1]));

  result.m[2][1] = 1 / det *
                   ((-1 * m.m[0][0] * m.m[2][1] * m.m[3][3]) -
                    (m.m[0][1] * m.m[2][3] * m.m[3][0]) -
                    (m.m[0][3] * m.m[2][0] * m.m[3][1]) +
                    (m.m[0][3] * m.m[2][1] * m.m[3][0]) +
                    (m.m[0][1] * m.m[2][0] * m.m[3][3]) +
                    (m.m[0][0] * m.m[2][3] * m.m[3][1]));

  result.m[2][2] = 1 / det *
                   ((m.m[0][0] * m.m[1][1] * m.m[3][3]) +
                    (m.m[0][1] * m.m[1][3] * m.m[3][0]) +
                    (m.m[0][3] * m.m[1][0] * m.m[3][1]) -
                    (m.m[0][3] * m.m[1][1] * m.m[3][0]) -
                    (m.m[0][1] * m.m[1][0] * m.m[3][3]) -
                    (m.m[0][0] * m.m[1][3] * m.m[3][1]));

  result.m[2][3] = 1 / det *
                   ((-1 * m.m[0][0] * m.m[1][1] * m.m[2][3]) -
                    (m.m[0][1] * m.m[1][3] * m.m[2][0]) -
                    (m.m[0][3] * m.m[1][0] * m.m[2][1]) +
                    (m.m[0][3] * m.m[1][1] * m.m[2][0]) +
                    (m.m[0][1] * m.m[1][0] * m.m[2][3]) +
                    (m.m[0][0] * m.m[1][3] * m.m[2][1]));

  result.m[3][0] = 1 / det *
                   ((-1 * m.m[1][0] * m.m[2][1] * m.m[3][2]) -
                    (m.m[1][1] * m.m[2][2] * m.m[3][0]) -
                    (m.m[1][2] * m.m[2][0] * m.m[3][1]) +
                    (m.m[1][2] * m.m[2][1] * m.m[3][0]) +
                    (m.m[1][1] * m.m[2][0] * m.m[3][2]) +
                    (m.m[1][0] * m.m[2][2] * m.m[3][1]));

  result.m[3][1] = 1 / det *
                   ((m.m[0][0] * m.m[2][1] * m.m[3][2]) +
                    (m.m[0][1] * m.m[2][2] * m.m[3][0]) +
                    (m.m[0][2] * m.m[2][0] * m.m[3][1]) -
                    (m.m[0][2] * m.m[2][1] * m.m[3][0]) -
                    (m.m[0][1] * m.m[2][0] * m.m[3][2]) -
                    (m.m[0][0] * m.m[2][2] * m.m[3][1]));

  result.m[3][2] = 1 / det *
                   ((-1 * m.m[0][0] * m.m[1][1] * m.m[3][2]) -
                    (m.m[0][1] * m.m[1][2] * m.m[3][0]) -
                    (m.m[0][2] * m.m[1][0] * m.m[3][1]) +
                    (m.m[0][2] * m.m[1][1] * m.m[3][0]) +
                    (m.m[0][1] * m.m[1][0] * m.m[3][2]) +
                    (m.m[0][0] * m.m[1][2] * m.m[3][1]));

  result.m[3][3] = 1 / det *
                   ((m.m[0][0] * m.m[1][1] * m.m[2][2]) +
                    (m.m[0][1] * m.m[1][2] * m.m[2][0]) +
                    (m.m[0][2] * m.m[1][0] * m.m[2][1]) -
                    (m.m[0][2] * m.m[1][1] * m.m[2][0]) -
                    (m.m[0][1] * m.m[1][0] * m.m[2][2]) -
                    (m.m[0][0] * m.m[1][2] * m.m[2][1]));

  return result;
}

Matrix4x4 Transpose(const Matrix4x4 &m) {
  Matrix4x4 result;

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      result.m[i][j] = m.m[j][i];
    }
  }

  return result;
}

Matrix4x4 MakeIdentity4x4() {
  Matrix4x4 result;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      if (i == j) {
        result.m[i][j] = 1.0f;
      } else {
        result.m[i][j] = 0.0f;
      }
    }
  }
  return result;
}

//==================================
// 回転行列
//==================================

Matrix4x4 MakeRotateMatrix(ShaftType shaft, float radian) {
  Matrix4x4 result;

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      result.m[i][j] = 0.0f;
    }
  }

  switch (shaft) {
  case X:

    result.m[0][0] = 1.0f;
    result.m[1][1] = std::cosf(radian);
    result.m[1][2] = std::sinf(radian);
    result.m[2][1] = -std::sinf(radian);
    result.m[2][2] = std::cosf(radian);
    result.m[3][3] = 1.0f;

    break;
  case Y:

    result.m[0][0] = std::cosf(radian);
    result.m[0][2] = -std::sinf(radian);
    result.m[1][1] = 1.0f;
    result.m[2][0] = std::sinf(radian);
    result.m[2][2] = std::cosf(radian);
    result.m[3][3] = 1.0f;

    break;
  case Z:

    result.m[0][0] = std::cosf(radian);
    result.m[0][1] = std::sinf(radian);
    result.m[1][0] = -std::sinf(radian);
    result.m[1][1] = std::cosf(radian);
    result.m[2][2] = 1.0f;
    result.m[3][3] = 1.0f;

    break;
  }

  return result;
}

//==================================
// 平行移動行列
//==================================

Matrix4x4 MakeTranslateMatrix(const Vector3 &translate) {

  Matrix4x4 result;

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      if (i == j) {
        result.m[i][j] = 1.0f;
      } else {
        result.m[i][j] = 0.0f;
      }
    }
  }

  result.m[3][0] = translate.x;
  result.m[3][1] = translate.y;
  result.m[3][2] = translate.z;

  return result;
}

//==================================
// 拡大縮小行列
//==================================

Matrix4x4 MakeScaleMatrix(const Vector3 &scale) {
  Matrix4x4 result;

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      if (i == j) {
        result.m[i][j] = (i == 0)   ? scale.x
                         : (i == 1) ? scale.y
                         : (i == 2) ? scale.z
                                    : 1.0f;
      } else {
        result.m[i][j] = 0.0f;
      }
    }
  }

  return result;
}

//==================================
// 補間関数
//==================================

Vector3 Lerp(const Vector3 &v1, const Vector3 &v2, float t) {
  Vector3 result;
  result.x = v1.x + (v2.x - v1.x) * t;
  result.y = v1.y + (v2.y - v1.y) * t;
  result.z = v1.z + (v2.z - v1.z) * t;
  return result;
}

Quaternion Slerp(const Quaternion &q1, const Quaternion &q2, float t) {
  float dot = q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w;
  Quaternion tempQ2 = q2;

  if (dot < 0.0f) {
    dot = -dot;
    tempQ2.x = -tempQ2.x;
    tempQ2.y = -tempQ2.y;
    tempQ2.z = -tempQ2.z;
    tempQ2.w = -tempQ2.w;
  }

  float scale0, scale1;

  if ((1.0f - dot) > 0.0001f) {
    float theta = std::acosf(dot);
    float sinTheta = std::sinf(theta);
    scale0 = std::sinf((1.0f - t) * theta) / sinTheta;
    scale1 = std::sinf(t * theta) / sinTheta;
  } else {
    scale0 = 1.0f - t;
    scale1 = t;
  }

  Quaternion result;
  result.x = q1.x * scale0 + tempQ2.x * scale1;
  result.y = q1.y * scale0 + tempQ2.y * scale1;
  result.z = q1.z * scale0 + tempQ2.z * scale1;
  result.w = q1.w * scale0 + tempQ2.w * scale1;
  return result;
}

//==================================
// アフィン変換
//==================================
Matrix4x4 MakeAffineMatrix(const Vector3 &scale, const Vector3 &rotate,
                           const Vector3 &translate) {

  // スケーリング行列 S
  Matrix4x4 resultScaleMatrix = MakeScaleMatrix(scale);

  // 回転行列 R
  Matrix4x4 resultRotateXMatrix = MakeRotateMatrix(X, rotate.x);
  Matrix4x4 resultRotateYMatrix = MakeRotateMatrix(Y, rotate.y);
  Matrix4x4 resultRotateZMatrix = MakeRotateMatrix(Z, rotate.z);
  Matrix4x4 resultRotateMatrix = Multiply(
      resultRotateXMatrix, Multiply(resultRotateYMatrix, resultRotateZMatrix));

  // 平行移動行列 T
  Matrix4x4 resultTranslateMatrix = MakeTranslateMatrix(translate);

  // アフィン変換行列 W = S * R * T
  Matrix4x4 result = Multiply(
      resultScaleMatrix, Multiply(resultRotateMatrix, resultTranslateMatrix));

  return result;
}

Matrix4x4 MakeRotateMatrix(const Quaternion& q) {
  Matrix4x4 result = MakeIdentity4x4();
  float xx = q.x * q.x;
  float yy = q.y * q.y;
  float zz = q.z * q.z;
  float xy = q.x * q.y;
  float xz = q.x * q.z;
  float yz = q.y * q.z;
  float wx = q.w * q.x;
  float wy = q.w * q.y;
  float wz = q.w * q.z;

  result.m[0][0] = 1.0f - 2.0f * (yy + zz);
  result.m[0][1] = 2.0f * (xy + wz);
  result.m[0][2] = 2.0f * (xz - wy);

  result.m[1][0] = 2.0f * (xy - wz);
  result.m[1][1] = 1.0f - 2.0f * (xx + zz);
  result.m[1][2] = 2.0f * (yz + wx);

  result.m[2][0] = 2.0f * (xz + wy);
  result.m[2][1] = 2.0f * (yz - wx);
  result.m[2][2] = 1.0f - 2.0f * (xx + yy);

  return result;
}

Vector3 QuaternionToEuler(const Quaternion& q) {
    Vector3 euler;
    // ロール（X 軸回転）
    float sinr_cosp = 2.0f * (q.w * q.x + q.y * q.z);
    float cosr_cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    euler.x = std::atan2(sinr_cosp, cosr_cosp);

    // ピッチ（Y 軸回転）
    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    if (std::abs(sinp) >= 1.0f)
        euler.y = std::copysign(3.14159265358979323846f / 2.0f, sinp); // 誤差で ±1 を超えると asin が NaN になるので ±90° に丸める
    else
        euler.y = std::asin(sinp);

    // ヨー（Z 軸回転）
    float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    euler.z = std::atan2(siny_cosp, cosy_cosp);

    return euler;
}

Matrix4x4 MakeAffineMatrix(const Vector3 &scale, const Quaternion &rotate, const Vector3 &translate) {
  Matrix4x4 resultScaleMatrix = MakeScaleMatrix(scale);
  Matrix4x4 resultRotateMatrix = MakeRotateMatrix(rotate);
  Matrix4x4 resultTranslateMatrix = MakeTranslateMatrix(translate);
  return Multiply(resultScaleMatrix, Multiply(resultRotateMatrix, resultTranslateMatrix));
}

//==================================
// 透視投影行列
//==================================

Matrix4x4 MakePerspectiveFovMatrix(float fov, float aspectRatio, float nearClip,
                                   float farClip) {
  Matrix4x4 result = {};
  float cot = 1.0f / std::tanf(fov / 2.0f);

  result.m[0][0] = cot / aspectRatio;
  result.m[1][1] = cot;
  result.m[2][2] = farClip / (farClip - nearClip);
  result.m[2][3] = 1.0f;
  result.m[3][2] = -(farClip * nearClip) / (farClip - nearClip);

  return result;
}

//==================================
// 正射影行列
//==================================

Matrix4x4 MakeOrthographicMatrix(float left, float top, float right,
                                 float bottom, float nearClip, float farClip) {

  Matrix4x4 result = {};

  result.m[0][0] = 2.0f / (right - left);
  result.m[1][1] = 2.0f / (top - bottom);
  result.m[2][2] = 1.0f / (farClip - nearClip);
  result.m[3][0] = (left + right) / (left - right);
  result.m[3][1] = (top + bottom) / (bottom - top);
  result.m[3][2] = nearClip / (nearClip - farClip);
  result.m[3][3] = 1.0f;

  return result;
}

//==================================
// ビューポート変換行列
//==================================

Matrix4x4 MakeViewportMatrix(float left, float top, float width, float height,
                             float minDepth, float maxDepth) {
  Matrix4x4 result = {};

  result.m[0][0] = width / 2.0f;
  result.m[1][1] = -height / 2.0f;
  result.m[2][2] = maxDepth - minDepth;
  result.m[3][0] = left + (width / 2.0f);
  result.m[3][1] = top + (height / 2.0f);
  result.m[3][2] = minDepth;
  result.m[3][3] = 1.0f;

  return result;
}
