#include "Arduino.h"
#include "misakiUTF16.h"

//-----------------------------------------------------------------------------
// LEDマトリクスによる電光掲示板もどき 8*32ドット
//=============================================================================
// 8*8マトリクスを横に四つ繋げた電光掲示板もどきを制御するプログラム
//-----------------------------------------------------------------------------
//【予約ピン】
// SDカードをSPI接続で使用するため、以下のピンをSPIライブラリで使用する。
// 4:CS
// 11:MOSI
// 12:MISO
// 13:CLK
// LEDマトリクス制御部への接続ピン
// 6:SER  (Data)
// 7:SRCLK(Shift)
// 8:RCLK (Latch)
// 9:SRCLR(Clear)
// 10:OE  (Enable)※論理反転なので注意
//-----------------------------------------------------------------------------
// 制限事項
// フォント周りをSDカード格納に移行する予定（現時点では未実装）
// digitalWriteの引数がuint_8tで宣言されているため、1送信8bitまでの制限がある
// 割り込みを使用していないため、シリアルから受信すると表示抑止となって何も表示されない。
// 受信が完了すると受信した文字列を表示する
// 文字列先頭とマトリクス先頭が同一なので先頭をいきなり表示した状態からスクロールが開始される
//-----------------------------------------------------------------------------

// 定数定義
const int serialData = 6;	// シリアルデータ
const int shiftClk = 7;  	// データシフトクロック
const int latchClk = 8;		// ラッチクロック
const int regClear = 9;    	// シフトレジスタクリア
const int outputEnable = 10;	// 出力切り替え

// 表示関連
const int cursorHeight = 8;		// カーソルの高さ
const int cursorWidth = 32; 	// カーソルの幅
const int colsInRow = cursorWidth - 1;
const int bitMask = 128;		// ビットマスク（上位１bitのみ）

const int scrollDelayTime = 50;	// 次に1列スクロールするまでの待機時間
uint32_t dispCursor[cursorHeight];	// 表示用カーソル
byte strBuffer[120][cursorHeight];	// 文字列全体を格納するバッファ領域(max:120文字)
int bufferLength = 0;			// バッファの長さ（=シリアル受信した文字列の文字列長）
const int fontDotHeight = 8;	// フォント1文字のドット高
const int fontDotWidth = 8;		// フォント1文字のドット幅

//*****************************************************************************
// 表示関連関数群
//*****************************************************************************
//-----------------------------------------------------------------------------
// 文字列をスクロールする
// 対象文字列はstrBufferにビットマップ変換の上格納すること
//-----------------------------------------------------------------------------
void scrollString() {
	unsigned long startTime = 0;	//カーソル内を表示する際の開始時間
	int startIndex = 0;				//カーソルで読み込む文字の文字内での先頭位置(nビット目)
	int startChar = 0;				//カーソルで読み込む文字の文字列内での先頭位置(n文字目)
	int currentIndex = 0;			//カーソルで読み込み中の文字内のビット位置
	int charIndex = 0;				//カーソルで読み込み中の文字列内の文字位置
	int shiftCount = 1;				//フェイドアウトの送り回数
	int bitCount = 0;				//バッファから読み込んだビット数
	bool isScrollEnd = false;		//スクロール完了フラグ

	clearDispCursor();

	// スクロールが終わるまでループ
	while ( !isScrollEnd ) {

		//文字列バッファの最後になるまではバッファから読み込む
		if ( charIndex < bufferLength ) {
			if (Serial.available()) {
				//なにがしか受信したらスクロール終了
				isScrollEnd = true;
				break;
			}

			// 現在列を開始列に設定する
			currentIndex = startIndex;

			// 今回の表示カーソルをクリアする
			clearDispCursor();

			//表示領域の列数分ループ
			bitCount = 0;
			while (bitCount < cursorWidth) {
				for ( int row = 0; row < fontDotHeight; row++ ) {
					//バッファ内各行の現在位置のビットを表示領域の指定位置へセット
					bitWrite(dispCursor[row],
							 colsInRow - bitCount,
							 bitRead(strBuffer[charIndex][row], fontDotWidth - currentIndex));
				}

				//現在位置を一つ後ろに設定
				currentIndex++;

				if ( currentIndex == fontDotWidth ) {
					// 現在位置が文字幅のビット数を超えたので、現在位置を先頭に戻して次の文字へ
					currentIndex = 0;
					charIndex++;
				}

				// 文字位置がバッファの長さを超えたら終了
				if ( charIndex >= bufferLength ) break;
				//読込ビット数を+1
				bitCount++;
			}

		} else {
			//文字列バッファ末尾の文字を読み込んだ後は単純にスクロール方向へビットシフトする
			for ( int row = 0; row < fontDotHeight; row++ ) {
				dispCursor[row] = dispCursor[row] << shiftCount;
			}
			shiftCount++;
		}

		//表示領域の編集が終わったのでLEDマトリクスへ反映
		startTime = millis();
		// スクロール１列辺りの表示時間を経過するまで同じデータを表示する
		while( millis() < startTime + scrollDelayTime ) {
			if (Serial.available()) {
				//なにがしか受信したらスクロール終了
				isScrollEnd = true;
				break;
			}

			// シフトレジスタへのデータ送信
			// フォントは行方向でスライスしたイメージで格納されているので、
			// 上段から下段へ向けて行数分の表示を行う
			for ( int row = 0; row < fontDotHeight; row++ ) {
				totalColShiftOut(dispCursor[row]);	//	列表示パターン出力
				rowShiftOut(bitMask >> row);		//	行スキャン

				updateStorage(); //データ送信が終わったのでストレージへラッチ
				delayMicroseconds(60); //次の行へ移る前に表示状態で待機
			}
		}


		// 1列スクロールしたので、文字のビット幅と比較する
		startIndex = (startIndex + 1) % fontDotWidth;

		// １文字のビット幅分スクロールしたので次回の開始文字を一つ後ろへずらす
		if (startIndex == 0) startChar++;

		// 次回読込時の先頭文字をセット
		charIndex = startChar;


		//カーソルが空（＝カーソルの幅分シフトした）になったら終了。
		if ( shiftCount >= cursorWidth ) {
			isScrollEnd = true;
		}
	}

	// 文字列受信
	receiveString();
}


//-----------------------------------------------------------------------------
// カーソル内データクリア
//-----------------------------------------------------------------------------
void clearDispCursor() {
	for ( int i = 0; i < cursorHeight; i++ ) {
		dispCursor[i] = 0;
	}
}

//------------------------------------------------------------------------------------
// 指定された文字列のフォントを取得してバッファに格納する
// 引数：char* pUTF8 任意の文字列（ポインタ渡し）
//------------------------------------------------------------------------------------
void getFontToBuffer(char* pUTF8) {
	int n = 0;

	while(*pUTF8) {
		pUTF8 = getFontData(&strBuffer[n++][0], pUTF8);
	}

	bufferLength = n;

}

//------------------------------------------------------------------------------------
// 起動時LEDマトリクスチェック。LEDを順番に一つずつ全て点灯する。
//------------------------------------------------------------------------------------
void checkAllLED() {
	uint32_t colData = 0X80000000;
	uint32_t buffer = 0;

	// 列側の送信回数 (digitalWriteの引数valのサイズから導かれる）
	int sendLimit = cursorWidth / 8;

	for ( int row = 0; row < cursorHeight; row++ ) {
		for ( int col = 0; col < cursorWidth; col++ ) {
			buffer = colData >> col;
			totalColShiftOut(buffer);

			rowShiftOut(bitMask >> row);

			updateStorage();
			delay(15);
		}
	}
}
//-----------------------------------------------------------------------------
// 列側データ一括出力
// digitalWriteのval引数がuint_8tなので、8bitずつ送信しなければならない
//-----------------------------------------------------------------------------
void totalColShiftOut( const uint32_t &data) {
	int sendLimit = cursorWidth / 8; // 全送信回数 カーソルのドット幅/8
	uint8_t targetByte = 0;		// 送信対象のデータを格納するバッファ

	// 送信回数分送信処理を行う
	for ( int sendCount = sendLimit; sendCount > 0; sendCount-- ) {
		// 送信できるビット幅分のデータをバッファに格納
		targetByte = data >> sendCount * 8;

		// バッファ内のデータを送信
		for ( int i = 0; i < 8; i++ ) {
			digitalWrite(shiftClk, LOW);
			digitalWrite(serialData, targetByte & bitMask >> i);
			digitalWrite(shiftClk, HIGH);
		}
	}
}

//-----------------------------------------------------------------------------
// 列側データ出力
// digitalWriteのval引数がuint_8tなので、8bitずつ送信しなければならない
//-----------------------------------------------------------------------------
void colShiftOut( const uint8_t &data ) {

	for ( int i = 0; i < 8; i++ ) {
		digitalWrite(shiftClk, LOW);
		digitalWrite(serialData, data & bitMask >> i);
		digitalWrite(shiftClk, HIGH);
	}
}

//-----------------------------------------------------------------------------
// 行側データ出力
//-----------------------------------------------------------------------------
void rowShiftOut( const uint8_t &data ) {

	for ( int i = 0; i < 8; i++ ) {
		digitalWrite(shiftClk, LOW);
		digitalWrite(serialData, data & bitMask >> i);
		digitalWrite(shiftClk, HIGH);
	}
}
//*****************************************************************************
// シリアル通信関連
//*****************************************************************************
//-----------------------------------------------------------------------------
// 文字列受信
//-----------------------------------------------------------------------------
void receiveString() {
	if ( Serial.available() ) {
		// なにがしか受信しているのであれば、受信文字列を受け取る
		String buffer;

		//終端文字が受信されるまで無限ループ
		while (1) {
			if ( Serial.available() ) {
				char temp = char(Serial.read());
				buffer += temp;

				//改行コードを１送信分の終端として処理
				if ( temp == '\r' ) {
					//受信文字列の前後の空白を取り除く
					buffer.trim();
					//文字列をフォントに変換する
					getFontToBuffer(buffer.c_str());
					break;
				}
			}
		}
	}
}

//*****************************************************************************
// シフトレジスタ制御関連関数群
//*****************************************************************************
//-----------------------------------------------------------------------------
// レジスタ（シフト・ストレージ共）クリア
//-----------------------------------------------------------------------------
void clearRegister() {
	digitalWrite(regClear, LOW);
	digitalWrite(regClear, HIGH);
	updateStorage();
}

//-----------------------------------------------------------------------------
// シフトレジスタをストレージレジスタにラッチする
//-----------------------------------------------------------------------------
void updateStorage() {
	digitalWrite(latchClk, LOW);
	digitalWrite(latchClk, HIGH);
}

//-----------------------------------------------------------------------------
// シフトレジスタからの出力を有効にする
//-----------------------------------------------------------------------------
void enableOutput() {
	digitalWrite(outputEnable, LOW);
}

//-----------------------------------------------------------------------------
// シフトレジスタからの出力を無効にする
//-----------------------------------------------------------------------------
void disableOutput() {
	digitalWrite(outputEnable, HIGH);
}

//*****************************************************************************
// 初期処理
//*****************************************************************************
void setup()
{
	// GPIOピンの設定
	pinMode(serialData, OUTPUT);
	pinMode(shiftClk, OUTPUT);
	pinMode(latchClk, OUTPUT);
	pinMode(regClear, OUTPUT);
	pinMode(outputEnable, OUTPUT);

	// レジスタクリア
	disableOutput();
	clearRegister();
	enableOutput();

	// LED全数チェック
	checkAllLED();


	// レジスタクリア
	disableOutput();
	clearRegister();
	enableOutput();

	// シリアル通信開始
	Serial.begin(9600);

	//初期表示文字列の返還後フォント列を取得
	getFontToBuffer("  　　春はあけぼの。やうやう白くなりゆく、山ぎはすこしあかりて、むらさきだちたる雲のほそくたなびきたる。");

}

//*****************************************************************************
// メイン処理
//*****************************************************************************
void loop()
{
	clearRegister();

	scrollString();

	delay(100);


}
